#!/usr/bin/env python3
"""
Build comparison tables from PicoBench RESULTS.CSV files of several boards.

Usage:
    worklog/tools/compare_runs.py LABEL=DIR [LABEL=DIR ...] > comparison.md

Each DIR is a directory copied from the PICOBENCH disk (or a RUNS/RUNxxxx folder).
Everything comes from RESULTS.CSV, so the tables can be regenerated for any run,
which is the point: "before and after" comparisons must not be retyped by hand.
"""

import csv
import sys
from pathlib import Path

# Latency values are cycles; ns is derived from the operating point in the report.
STAT_COLS = {"min": 11, "p50": 12, "p95": 13, "p99": 14, "p99_9": 15, "max": 16}


def load(path: Path):
    """rows[(test, label, op_mhz, record)] = dict(column -> value)"""
    rows = {}
    with (path / "RESULTS.CSV").open() as f:
        reader = csv.reader(f)
        header = next(reader)
        for r in reader:
            if len(r) < len(header):
                continue
            d = dict(zip(header, r))
            rows[(d["test"], d["label"], d["op_mhz"], d["record"])] = d
    return rows


def stat(rows, test, label, op, field="p50"):
    d = rows.get((test, label, str(op), "stats"))
    return int(d[field]) if d and d[field] else None


def value(rows, test, label, op):
    d = rows.get((test, label, str(op), "value"))
    return int(d["value"]) if d and d["value"] else None


def ns(cycles, mhz):
    return None if cycles is None else cycles * 1000.0 / mhz


def fmt(v, digits=0):
    if v is None:
        return "-"
    return f"{v:.{digits}f}" if digits else f"{v:.0f}"


def table(title, headers, lines):
    out = [f"\n### {title}\n", "| " + " | ".join(headers) + " |",
           "|" + "|".join("---" for _ in headers) + "|"]
    out.extend("| " + " | ".join(line) + " |" for line in lines)
    return "\n".join(out)


def ops_of(rows):
    return sorted({int(k[2]) for k in rows if k[2].isdigit()})


def default_op(rows):
    """The operating point the whole suite ran at (the swept ones carry fewer tests)."""
    per_op = {}
    for test, _label, op, _record in rows:
        if op.isdigit():
            per_op.setdefault(int(op), set()).add(test)
    return max(per_op, key=lambda op: len(per_op[op])) if per_op else 0


def main():
    boards = {}
    for arg in sys.argv[1:]:
        label, _, path = arg.partition("=")
        boards[label] = load(Path(path))
    if not boards:
        print(__doc__)
        return 1

    print("# PicoBench comparison\n")
    for label, rows in boards.items():
        any_row = next(iter(rows.values()))
        print(f"- **{label}**: {any_row['chip']} / {any_row['arch']}, "
              f"build {any_row['build_id']}, operating points {ops_of(rows)} MHz")

    # ---- Hot path: the cartridge measurement -------------------------------------------
    variants = [
        ("sram_table_sram_code", "table in SRAM, code in SRAM"),
        ("sram_table_flash_code", "table in SRAM, code in flash"),
        ("flash_table_warm", "table in flash, cache warm"),
        ("flash_table_cold_cache", "table in flash, cache cold"),
        ("flash_table_uncached", "table in flash, uncached"),
        ("psram_table_cached", "table in PSRAM, cached"),
        ("psram_table_uncached", "table in PSRAM, uncached"),
    ]
    for label, rows in boards.items():
        ops = ops_of(rows)
        top = max(ops) if ops else 0
        lines = []
        for v, desc in variants:
            p50 = stat(rows, "hotpath.memory", v, top)
            if p50 is None:
                continue
            lines.append([desc,
                          fmt(p50), fmt(stat(rows, "hotpath.memory", v, top, "p99_9")),
                          fmt(stat(rows, "hotpath.memory", v, top, "max")),
                          fmt(ns(p50, top), 1),
                          fmt(ns(stat(rows, "hotpath.memory", v, top, "max"), top), 1)])
        if lines:
            print(table(f"Cartridge hot path - {label} at {top} MHz",
                        ["variant", "p50 [cyc]", "p99.9 [cyc]", "max [cyc]", "p50 [ns]", "max [ns]"],
                        lines))

    # ---- Hot path against the CPU clock --------------------------------------------------
    for label, rows in boards.items():
        ops = ops_of(rows)
        lines = []
        for v, desc in variants:
            if stat(rows, "hotpath.memory", v, max(ops)) is None:
                continue
            row = [desc]
            for op in ops:
                c = stat(rows, "hotpath.memory", v, op)
                row.append(f"{fmt(c)} / {fmt(ns(c, op), 0)}" if c else "-")
            lines.append(row)
        if lines:
            print(table(f"Hot path against clk_sys - {label} (cycles / ns)",
                        ["variant"] + [f"{op} MHz" for op in ops], lines))

    # ---- Memory access latency ------------------------------------------------------------
    # (description, test, label, accesses per sample); millicycle values recorded with
    # rec_value() are marked with per=None and divided by 1000 instead.
    sources = [
        ("SRAM, dependent load", "sram.latency", "chase_4k", 1024),
        ("PSRAM cached, dependent load", "psram.bench", "chase_cached_16k_mcyc", None),
        ("PSRAM uncached, dependent load", "psram.bench", "chase_uncached_16k", 256),
        ("PSRAM single read, uncached", "psram.bench", "read_uncached_single", 1),
        ("PSRAM cache miss", "psram.bench", "read_miss_cold", 1),
        ("flash cache hit", "flash.cache", "read_hit", 1),
        ("flash cache miss (cold)", "flash.cache", "read_miss_cold", 1),
    ]
    for label, rows in boards.items():
        ops = ops_of(rows)
        lines = []
        for desc, test, lbl, per in sources:
            row = [desc]
            found = False
            for op in ops:
                if per is None:
                    v = value(rows, test, lbl, op)
                    per_access = v / 1000.0 if v is not None else None
                else:
                    c = stat(rows, test, lbl, op)
                    per_access = c / per if c is not None else None
                if per_access is None:
                    row.append("-")
                    continue
                found = True
                row.append(f"{per_access:.2f} / {per_access * 1000.0 / op:.1f}")
            if found:
                lines.append(row)
        if lines:
            print(table(f"Access latency per read - {label} (cycles / ns)",
                        ["memory"] + [f"{op} MHz" for op in ops], lines))

    # ---- Throughput -------------------------------------------------------------------------
    rates = [
        ("SRAM read 32-bit", "sram.throughput", "g0_read32_seq_rate"),
        ("SRAM write 32-bit", "sram.throughput", "g0_write32_seq_rate"),
        ("flash XIP streaming", "flash.read", "cached_stream_read32_64k_rate"),
        ("flash uncached", "flash.read", "uncached_read32_64k_rate"),
        ("PSRAM read uncached", "psram.bench", "uncached_read32_64k_rate"),
        ("PSRAM write uncached", "psram.bench", "uncached_write32_64k_rate"),
        ("PSRAM read cached (4 KB)", "psram.bench", "cached_read32_4k_rate"),
    ]
    for label, rows in boards.items():
        ops = ops_of(rows)
        lines = []
        for desc, test, lbl in rates:
            row = [desc]
            found = False
            for op in ops:
                v = value(rows, test, lbl, op)
                if v is None:
                    row.append("-")
                    continue
                found = True
                row.append(f"{v / 1000.0:.1f}")
            if found:
                lines.append(row)
        if lines:
            print(table(f"Throughput - {label} (MB/s)",
                        ["path"] + [f"{op} MHz" for op in ops], lines))

    # ---- Hot path under load -------------------------------------------------------------------
    disturb = [
        ("sram_quiet", "SRAM table, quiet"),
        ("sram_irqs_10khz", "SRAM table, 10 kHz interrupt"),
        ("sram_dma_sram", "SRAM table, SRAM DMA"),
        ("sram_table_dma_psram", "SRAM table, PSRAM DMA"),
        ("flash_table_quiet", "flash table, quiet"),
        ("flash_table_dma_sram", "flash table, SRAM DMA"),
        ("flash_table_dma_psram", "flash table, PSRAM DMA"),
    ]
    for label, rows in boards.items():
        top = default_op(rows)   # this test is not swept: it runs at the default clock
        lines = []
        for v, desc in disturb:
            p50 = stat(rows, "hotpath.disturbance", v, top)
            if p50 is None:
                continue
            lines.append([desc, fmt(p50),
                          fmt(stat(rows, "hotpath.disturbance", v, top, "p99")),
                          fmt(stat(rows, "hotpath.disturbance", v, top, "p99_9")),
                          fmt(stat(rows, "hotpath.disturbance", v, top, "max")),
                          fmt(ns(stat(rows, "hotpath.disturbance", v, top, "max"), top), 1)])
        if lines:
            print(table(f"Hot path under load - {label} at {top} MHz",
                        ["variant", "p50 [cyc]", "p99 [cyc]", "p99.9 [cyc]", "max [cyc]", "max [ns]"],
                        lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
