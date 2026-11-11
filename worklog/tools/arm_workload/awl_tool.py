#!/usr/bin/env python3
"""awl_tool - build PicoBench's ARM workload from your own game ROMs.

The cpu.thumb_interp and hotpath.arm_neighbour tests replay real ARM code of
Harmony cartridge games (DPC+, CDF, CDFJ, CDFJ+) through a Thumb interpreter.
That code belongs to the games' authors, so it is never part of the repository:
this tool records it from ROM files you have, in the Gopher2600 emulator, and
writes workload/arm_workload.c (git-ignored). Without that file the firmware
builds anyway and the two tests report N/A.

usage:
  awl_tool.py list                      the catalogue (titles.csv)
  awl_tool.py scan PATH...              which catalogue titles these ROMs are
  awl_tool.py build PATH... [options]   record and write the workload

PATH is a ROM file or a directory (searched recursively for *.bin / *.a26).
ROMs are identified by the CRC32 of the whole file, never by name.

build options:
  (none)                the reference set - the titles PicoBench's own results use
  --titles a,b,c        these catalogue titles; --titles all = every one found
  --crc 4f8184d7,...    the ROMs with these CRC32s (catalogue or not)
  --add ROM[:NAME[:MIPS]]
                        any other ARM cartridge; NAME defaults to the file name,
                        MIPS (the rate it needs for a stable frame) to unknown
  --sweep               measure MIPS for --add/--crc titles without one (slow:
                        up to ten 3600-frame runs per title)
  --allow-different     accept catalogue titles whose recording differs from
                        the reference checksum (another ROM revision or emulator)
  -o FILE               output (default workload/arm_workload.c)
  --armstat PATH        recorder (default $PICOBENCH_ARMSTAT or
                        workload/.tools/armstat, see recorder/build_recorder.sh)
  --keep DIR            keep the recordings (NAME.rec, NAME.pages, NAME.txt)

How a title is recorded: armstat runs the ROM headless with scripted input
(RESET, FIRE, then random joystick moves from a fixed seed, so every run is the
same) and records `calls` consecutive ARM calls from frame `rec_from` on:
registers, cartridge RAM before and after, every driver call and peripheral
read. tools replay.c then replays every call through src/tests/arm/thumb_core.c
bit for bit and keeps only the 256-byte ROM pages the calls need. For a
catalogue title the result must match `title_crc32`, i.e. be identical to the
workload PicoBench's published results were measured with.

Blob layout (little-endian 32-bit words, everything 4-byte aligned), read by
src/tests/arm/awl.c:
  'AWL1', ntitles, title_offset[ntitles]  (byte offsets from the blob start)
  title: name[16], format[8], need_mips_x10, rom_size, ram_size,
         exit_addr, npages, npages x {page_index, 256 data bytes}, ncalls,
         then per call: regs_in[16], flags_in (NZCV in bits 31..28),
         npatch, npatch x {offset, length, data padded to 4},
         nev, nev x 6 words (event: 1 = driver call, 2 = peripheral read),
         regs_out[15], flags_out, instructions, crc32 of RAM after the call.
  RAM before call 0 is all zeroes plus call 0's patches; RAM before call k is
  RAM after call k-1 plus call k's patches (the console's 6502 writes between
  two ARM calls).
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CATALOGUE = HERE / "titles.csv"
THUMB = ROOT / "src" / "tests" / "arm"
TOOLS = ROOT / "workload" / ".tools"
DEFAULT_OUT = ROOT / "workload" / "arm_workload.c"

# Recording window for titles outside the catalogue (the reference set uses the same).
DEFAULT_FRAMES, DEFAULT_FROM, DEFAULT_CALLS = 2500, 2400, 10
# MIPS sweep: CHAMP_ARM.md section 3 in PicoA10400 (tools/arm_carts/sweep.sh).
SWEEP_FRAMES = 3600
SWEEP_CLOCKS = [50, 40, 35, 30, 25, 20, 15, 10, 7]
ARM_FORMATS = {"DPC+", "CDF0", "CDF1", "CDFJ", "CDFJ+"}
ROM_SUFFIXES = {".bin", ".a26"}
ROM_MAX_BYTES = 1 << 20


class ToolError(Exception):
    pass


@dataclass
class Title:
    name: str
    set: str = ""
    format: str = ""
    rom_crc32: int = 0
    rom_size: int = 0
    need_mips: float = 0.0          # 0 = unknown
    frames: int = DEFAULT_FRAMES
    rec_from: int = DEFAULT_FROM
    calls: int = DEFAULT_CALLS
    title_crc32: int = 0            # 0 = no reference to compare with
    title: str = ""
    rom: Path | None = None
    sweep: bool = False


def load_catalogue() -> list[Title]:
    out = []
    with CATALOGUE.open() as f:
        for r in csv.DictReader(line for line in f if not line.startswith("#")):
            out.append(Title(
                name=r["name"], set=r["set"], format=r["format"],
                rom_crc32=int(r["rom_crc32"], 16), rom_size=int(r["rom_size"]),
                need_mips=float(r["need_mips"] or 0), frames=int(r["frames"]),
                rec_from=int(r["rec_from"]), calls=int(r["calls"]),
                title_crc32=int(r["title_crc32"], 16), title=r["title"]))
    return out


def crc32_file(path: Path) -> int:
    return zlib.crc32(path.read_bytes()) & 0xFFFFFFFF


def find_roms(paths: list[str]) -> dict[int, Path]:
    """CRC32 -> first file with that content."""
    files = []
    for p in map(Path, paths):
        if p.is_dir():
            files += sorted(f for f in p.rglob("*") if f.is_file() and f.suffix.lower() in ROM_SUFFIXES)
        elif p.is_file():
            files.append(p)
        else:
            raise ToolError(f"{p}: no such file or directory")
    found: dict[int, Path] = {}
    for f in files:
        if f.stat().st_size <= ROM_MAX_BYTES:
            found.setdefault(crc32_file(f), f)
    return found


# ---- external tools ----

def armstat_path(arg: str | None) -> Path:
    p = Path(arg or os.environ.get("PICOBENCH_ARMSTAT") or TOOLS / "armstat")
    if not p.is_file():
        raise ToolError(f"recorder not found at {p}; build it with "
                        f"{(HERE / 'recorder' / 'build_recorder.sh').relative_to(ROOT)}")
    return p


def replay_path() -> Path:
    """Host build of replay.c against the firmware's own thumb_core, rebuilt when stale."""
    exe = TOOLS / "replay"
    srcs = [HERE / "replay.c", THUMB / "thumb_core.c", THUMB / "thumb_core.h"]
    if not exe.exists() or any(s.stat().st_mtime > exe.stat().st_mtime for s in srcs):
        TOOLS.mkdir(parents=True, exist_ok=True)
        cc = os.environ.get("CC", "cc")
        subprocess.run([cc, "-O2", "-Wall", "-Wno-unused-label", f"-I{THUMB}", "-o", str(exe),
                        str(HERE / "replay.c"), str(THUMB / "thumb_core.c")], check=True)
    return exe


def run_armstat(armstat: Path, t: Title, clock: int | None = None, rec: Path | None = None,
                log: Path | None = None) -> dict:
    env = dict(os.environ)
    frames = SWEEP_FRAMES if rec is None else t.frames
    if rec is not None:
        env.update(ARMREC=str(rec), ARMREC_FROM=str(t.rec_from), ARMREC_FIRST=str(t.calls),
                   ARMREC_MAX=str(t.calls))
    args = [str(armstat), str(t.rom), str(frames), "play"] + ([str(clock)] if clock else [])
    p = subprocess.run(args, env=env, capture_output=True, text=True)
    out = p.stdout + p.stderr
    if log:
        log.write_text(out)
    if p.returncode:
        raise ToolError(f"{t.name}: armstat failed:\n{out[-2000:]}")

    def grab(pattern, conv=str):
        m = re.search(pattern, out)
        return conv(m.group(1)) if m else None
    return {"mapper": grab(r"mapper=(\S+)"), "calls": grab(r"recorded calls=(\d+)", int),
            "offframes": grab(r"offframes=(\d+)/", int), "mips": grab(r"effMIPS=([0-9.]+)", float)}


def sweep(armstat: Path, t: Title) -> float:
    """effMIPS at the slowest ARM clock that breaks no more frames than 70 MHz does."""
    base = run_armstat(armstat, t, 70)
    need = base["mips"]
    for clk in SWEEP_CLOCKS:
        r = run_armstat(armstat, t, clk)
        if r["offframes"] > base["offframes"]:
            break
        need = r["mips"]
    return need or 0.0


# ---- recording -> blob ----

def load_rec(path: Path):
    b = path.read_bytes()
    if b[:8] != b"ARMREC01":
        raise ToolError(f"{path}: not an ARMREC01 recording")
    o = 8
    (n,) = struct.unpack_from("<I", b, o)
    o += 4
    segs = []
    for _ in range(n):
        segs.append(struct.unpack_from("<III", b, o))
        o += 12
    rom = bytearray()
    ram_size = 0
    for org, size, fl in segs:
        if fl & 1:
            ram_size += size
        else:
            if org != len(rom):
                raise ToolError(f"{path}: ROM segments are not contiguous from 0")
            rom += b[o:o + size]
            o += size
    entry = struct.unpack_from("<III", b, o)
    o += 12
    calls = []
    while o < len(b):
        if b[o:o + 4] != b"CALL":
            raise ToolError(f"{path}: bad call record at {o}")
        o += 4 + 8
        regs_in = struct.unpack_from("<16I", b, o); o += 64
        (flags_in,) = struct.unpack_from("<I", b, o); o += 4
        ram_in = b[o:o + ram_size]; o += ram_size
        (nev,) = struct.unpack_from("<I", b, o); o += 4
        events = [struct.unpack_from("<6I", b, o + 24 * i) for i in range(nev)]
        o += 24 * nev
        regs_out = struct.unpack_from("<16I", b, o); o += 64
        (flags_out,) = struct.unpack_from("<I", b, o); o += 4
        (count,) = struct.unpack_from("<Q", b, o); o += 8
        (status,) = struct.unpack_from("<I", b, o); o += 4
        ram_out = b[o:o + ram_size]; o += ram_size
        if status:
            raise ToolError(f"{path}: a recorded call did not end normally")
        calls.append((regs_in, flags_in, ram_in, events, regs_out, flags_out, count, ram_out))
    return bytes(rom), ram_size, entry, calls


def patches(base: bytes, new: bytes, gap: int = 8):
    """Runs of bytes where new differs from base; runs closer than `gap`
    bytes are merged (a patch header costs 8 bytes)."""
    out = []
    i, n = 0, len(new)
    while i < n:
        if base[i] == new[i]:
            i += 1
            continue
        j = last = i
        while j < n and j - last <= gap:
            if base[j] != new[j]:
                last = j
            j += 1
        out.append((i, new[i:last + 1]))
        i = last + 1
    return out


def w(*vals) -> bytes:
    return struct.pack("<%dI" % len(vals), *vals)


def pad4(b: bytes) -> bytes:
    return b + b"\0" * (-len(b) % 4)


def title_blob(t: Title, rec: Path, pages_file: Path):
    rom, ram_size, entry, calls = load_rec(rec)
    pages = [int(x) for x in pages_file.read_text().split()]
    b = t.name.encode().ljust(16, b"\0") + t.format.encode().ljust(8, b"\0")
    b += w(int(round(t.need_mips * 10)), len(rom), ram_size, entry[1] & ~1, len(pages))
    for p in pages:
        b += w(p) + rom[256 * p:256 * p + 256].ljust(256, b"\0")
    b += w(len(calls))
    ram = bytes(ram_size)
    for regs_in, flags_in, ram_in, events, regs_out, flags_out, count, ram_out in calls:
        b += w(*regs_in) + w(flags_in)
        pl = patches(ram, ram_in)
        b += w(len(pl))
        for off, data in pl:
            b += w(off, len(data)) + pad4(data)
        b += w(len(events))
        for e in events:
            b += w(*e)
        b += w(*regs_out[:15]) + w(flags_out, count, zlib.crc32(ram_out) & 0xFFFFFFFF)
        ram = ram_out
    return b, {"pages": len(pages), "instructions": sum(c[6] for c in calls), "calls": len(calls),
               "rom_size": len(rom), "ram_size": ram_size}


def make_title(t: Title, armstat: Path, replay: Path, work: Path) -> tuple[bytes, dict]:
    rec, pages, log = work / f"{t.name}.rec", work / f"{t.name}.pages", work / f"{t.name}.txt"
    st = run_armstat(armstat, t, rec=rec, log=log)
    if st["mapper"] not in ARM_FORMATS:
        raise ToolError(f"{t.name}: {t.rom} is a {st['mapper']} cartridge, not an ARM one")
    if not st["calls"]:
        raise ToolError(f"{t.name}: no ARM calls between frames {t.rec_from} and {t.frames}")
    if st["calls"] < t.calls:
        raise ToolError(f"{t.name}: only {st['calls']} of {t.calls} ARM calls recorded")
    if not t.format:
        t.format = st["mapper"]
    elif t.format != st["mapper"]:
        raise ToolError(f"{t.name}: the emulator sees {st['mapper']}, the catalogue says {t.format}")
    for args, what in (([str(rec)], "replay"), (["-m", str(pages), str(rec)], "page reduction")):
        p = subprocess.run([str(replay)] + args, capture_output=True, text=True)
        if p.returncode:
            raise ToolError(f"{t.name}: {what} failed - the interpreter does not reproduce "
                            f"Gopher2600:\n{p.stdout}{p.stderr}")
    if t.sweep and not t.need_mips:
        t.need_mips = sweep(armstat, t)
    return title_blob(t, rec, pages)


# ---- output ----

def write_c(out: Path, titles: list[Title], blobs: list[bytes]) -> int:
    head = b"AWL1" + w(len(blobs))
    off = len(head) + 4 * len(blobs)
    offsets = []
    for b in blobs:
        offsets.append(off)
        off += len(b)
    blob = head + w(*offsets) + b"".join(blobs)
    words = struct.unpack("<%dI" % (len(blob) // 4), blob)
    wid = zlib.crc32(blob) & 0xFFFFFFFF
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        f.write("// GENERATED by worklog/tools/arm_workload/awl_tool.py - do not edit.\n"
                "//\n"
                "// Excerpts of the ARM code and data of the games below (only the ROM pages\n"
                "// the recorded calls need) and the cartridge RAM they build at run time,\n"
                "// with Gopher2600's results for consecutive ARM calls of each.\n"
                "// THIS IS THIRD-PARTY GAME DATA: keep it local, do not publish it or UF2s\n"
                "// built with it (build.sh names those *_local.uf2).\n"
                "//\n"
                f"// workload crc32 {wid:08x}\n"
                "//   name         format  ROM crc32  need MIPS  title crc32\n")
        for t, b in zip(titles, blobs):
            need = f"{t.need_mips:9.1f}" if t.need_mips else "  unknown"
            f.write(f"//   {t.name:12s} {t.format:6s}  {t.rom_crc32:08x}  {need}  "
                    f"{zlib.crc32(b) & 0xFFFFFFFF:08x}\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"const uint32_t g_arm_workload_words = {len(words)}u;\n")
        f.write("const uint32_t g_arm_workload[] = {\n")
        for i in range(0, len(words), 8):
            f.write("    " + ", ".join(f"0x{x:08x}" for x in words[i:i + 8]) + ",\n")
        f.write("};\n")
    return wid


# ---- commands ----

def cmd_list(_args) -> int:
    for t in load_catalogue():
        need = f"{t.need_mips:4.1f}" if t.need_mips else "   ?"
        print(f"{t.name:12s} {t.set or '-':4s} {t.format:6s} rom {t.rom_crc32:08x} {t.rom_size // 1024:3d} KB  "
              f"need {need} MIPS  {t.title}")
    return 0


def cmd_scan(args) -> int:
    roms = find_roms(args.paths)
    cat = load_catalogue()
    by_crc = {t.rom_crc32: t for t in cat}
    for crc, path in sorted(roms.items(), key=lambda kv: str(kv[1])):
        t = by_crc.get(crc)
        if t or args.verbose:
            print(f"{crc:08x}  {t.name if t else '-':12s}  {path}")
    found = [t for t in cat if t.rom_crc32 in roms]
    ref = [t for t in cat if t.set == "ref"]
    missing = [t.name for t in ref if t.rom_crc32 not in roms]
    print(f"\n{len(roms)} ROM files, {len(found)} of {len(cat)} catalogue titles; reference set "
          + ("complete" if not missing else f"missing {', '.join(missing)}"))
    return 0


def parse_add(spec: str) -> Title:
    parts = spec.split(":")
    rom = Path(parts[0])
    if not rom.is_file():
        raise ToolError(f"{rom}: no such file")
    name = parts[1] if len(parts) > 1 and parts[1] else rom.stem
    t = Title(name=name, rom=rom, rom_crc32=crc32_file(rom), rom_size=rom.stat().st_size)
    if len(parts) > 2 and parts[2]:
        t.need_mips = float(parts[2])
    return t


def clean_name(name: str) -> str:
    # The firmware prints "<name>_<what>" labels with at most 12 name characters.
    return (re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "title")[:12]


def cmd_build(args) -> int:
    armstat = armstat_path(args.armstat)
    replay = replay_path()
    cat = load_catalogue()
    roms = find_roms(args.paths) if args.paths else {}
    by_crc = {t.rom_crc32: t for t in cat}
    jobs: list[Title] = []

    if args.titles:
        want = [t for t in cat if args.titles == "all" or t.name in args.titles.split(",")]
        unknown = set(args.titles.split(",")) - {t.name for t in cat} - {"all"}
        if unknown:
            raise ToolError(f"not in the catalogue: {', '.join(sorted(unknown))} (see: awl_tool.py list)")
        if args.titles == "all":
            want = [t for t in want if t.rom_crc32 in roms]
    elif args.crc or args.add:
        want = []
    else:
        want = [t for t in cat if t.set == "ref"]
    for crc_s in (args.crc.split(",") if args.crc else []):
        crc = int(crc_s, 16)
        if crc in by_crc:
            want.append(by_crc[crc])
        elif crc in roms:
            jobs.append(Title(name=roms[crc].stem, rom=roms[crc], rom_crc32=crc,
                              rom_size=roms[crc].stat().st_size, sweep=args.sweep))
        else:
            raise ToolError(f"no ROM with CRC32 {crc:08x} under {', '.join(args.paths) or '(no PATH given)'}")
    missing = [t.name for t in want if t.rom_crc32 not in roms]
    if missing:
        raise ToolError(f"ROM not found for: {', '.join(missing)} (identified by CRC32, see: awl_tool.py scan)")
    for t in want:
        t.rom = roms[t.rom_crc32]
    for spec in args.add or []:
        t = parse_add(spec)
        t.sweep = args.sweep
        if t.rom_crc32 in by_crc:
            print(f"note: {t.rom} is catalogue title {by_crc[t.rom_crc32].name}; using its entry")
            want.append(by_crc[t.rom_crc32])
            want[-1].rom = t.rom
        else:
            jobs.append(t)
    jobs = want + jobs
    for t in jobs:
        t.name = clean_name(t.name)
    names = [t.name for t in jobs]
    dup = {n for n in names if names.count(n) > 1}
    if dup:
        raise ToolError(f"duplicate title names: {', '.join(sorted(dup))} (name them with --add ROM:NAME)")
    if not jobs:
        raise ToolError("nothing to build")

    work = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="awl_"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            futures = [ex.submit(make_title, t, armstat, replay, work) for t in jobs]
            results = []
            for f in futures:
                try:
                    results.append(f.result())
                except ToolError as e:
                    results.append(e)
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)

    errors = [r for r in results if isinstance(r, ToolError)]
    for t, r in zip(jobs, results):
        if isinstance(r, ToolError):
            continue
        blob, info = r
        crc = zlib.crc32(blob) & 0xFFFFFFFF
        if t.title_crc32:
            ok = crc == t.title_crc32
            verdict = "identical to the reference" if ok else f"DIFFERS from the reference {t.title_crc32:08x}"
            if not ok and not args.allow_different:
                errors.append(ToolError(f"{t.name}: recording differs from the reference "
                                        f"(ROM revision or recorder version?); --allow-different accepts it"))
        else:
            verdict = "not in the catalogue"
        need = f"{t.need_mips:.1f}" if t.need_mips else "?"
        print(f"{t.name:12s} {t.format:6s} {info['calls']:2d} calls {info['instructions']:7d} instr "
              f"{info['pages']:3d} ROM pages {len(blob):6d} bytes  need {need:>4s} MIPS  "
              f"crc {crc:08x} {verdict}")
        if not t.title_crc32:
            print(f"  catalogue line: {t.name},,{t.format},{t.rom_crc32:08x},{t.rom_size},"
                  f"{t.need_mips or ''},{t.frames},{t.rec_from},{t.calls},{crc:08x},{t.rom.name}")
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 1
    blobs = [r[0] for r in results]
    out = Path(args.output)
    wid = write_c(out, jobs, blobs)
    print(f"workload {wid:08x}: {len(jobs)} titles, {sum(map(len, blobs)) // 1024} KB -> {out}")
    if out.resolve() != DEFAULT_OUT.resolve():
        print("note: the firmware picks it up with ./build.sh only from workload/arm_workload.c "
              "(or cmake -DPICOBENCH_ARM_WORKLOAD=PATH)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="Run with -h after a command for its options; details at the top of this file.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="show the catalogue")
    sp = sub.add_parser("scan", help="identify ROMs by CRC32")
    sp.add_argument("paths", nargs="+")
    sp.add_argument("-v", "--verbose", action="store_true", help="also list ROMs not in the catalogue")
    bp = sub.add_parser("build", help="record the ROMs and write the workload")
    bp.add_argument("paths", nargs="*")
    bp.add_argument("--titles")
    bp.add_argument("--crc")
    bp.add_argument("--add", action="append", metavar="ROM[:NAME[:MIPS]]")
    bp.add_argument("--sweep", action="store_true")
    bp.add_argument("--allow-different", action="store_true")
    bp.add_argument("-o", "--output", default=str(DEFAULT_OUT))
    bp.add_argument("--armstat")
    bp.add_argument("--keep")
    args = ap.parse_args()
    try:
        return {"list": cmd_list, "scan": cmd_scan, "build": cmd_build}[args.cmd](args)
    except ToolError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
