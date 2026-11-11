/*
 * ARM (Thumb) interpreter on real cartridge code - for the PicoA10400 question
 * "can this chip run the ARM code of Harmony cartridge games (DPC+, CDF, CDFJ,
 * CDFJ+) fast enough?" (PicoA10400 CHAMP_ARM.md, CHAMP_ARM_TODO.md stage P5).
 *
 * What runs: the interpreter from PicoA10400 tools/arm_carts/thumb (verbatim
 * copy in src/tests/arm/), direct-threaded, -O2, code and dispatch table in
 * SRAM. What it runs: ten consecutive ARM calls of each of seven games, recorded
 * in the Gopher2600 emulator while the game was being played, with only the ROM
 * pages those calls need (workload/arm_workload.c, generated from the user's own ROMs
 * by worklog/tools/arm_workload/awl_tool.py; N/A when the firmware has none). Every call's result -
 * registers, flags, instruction count and a CRC of the cartridge RAM - is
 * compared with Gopher2600's, so a wrong answer is an error, not a number.
 *
 * What it reports: cycles per guest instruction (the chip-independent figure),
 * the MIPS that gives at this clock and at the cartridge's 250 MHz, and whether
 * that is enough for each game: the "need" figure is the slowest ARM with which
 * the game still keeps a stable frame, measured in PicoA10400 with
 * tools/arm_carts/sweep.sh. Swept over the operating points, the CPI should not
 * change - that is what makes the 250 MHz projection a projection rather than
 * a guess (phase 4 showed SRAM costs in cycles are clock-independent).
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/clocks_ctl.h"
#include "core/guard.h"
#include "core/timing.h"
#include "hardware/sync.h"
#include "results/record.h"
#include "tests/arm/awl.h"

#define REPEATS 3u
#define CART_KHZ 250000u    // the clock PicoA10400 and Pico2A10400 run games at

// One call, timed, interrupts off. Lives in SRAM like the interpreter it calls.
static uint32_t __no_inline_not_in_flash_func(timed_run)(thumb_cpu_t *cpu, uint32_t max, uint32_t *cycles) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = timing_now();
    uint32_t st = thumb_run(cpu, max);
    uint32_t t1 = timing_now();
    restore_interrupts(irq);
    *cycles += timing_net(t0, t1);
    return st;
}

static void key(char *out, size_t n, const char *name, const char *what) {
    snprintf(out, n, "%.12s_%s", name, what);
}

test_result_t test_cpu_thumb_interp(test_ctx_t *ctx) {
    unsigned n = awl_ntitles();
    if (n == 0) {
        rec_text(ctx, "workload", "none built in - it holds game code, so it is generated locally "
                                  "from your own ROMs (workload/README.md)");
        return TR_NA;
    }
    // Full detail at the default operating point only. At the others the CPI per
    // game is what matters (it should not move), and the RP2040's record arena is
    // not large enough for everything five times over.
    const bool full = ctx->op == clocks_default_op();
    if (full) {
        rec_text(ctx, "interpreter", "thumb_core (PicoA10400 tools/arm_carts/thumb): ARMv4T Thumb, "
                                     "direct-threaded, -O2, code and dispatch table in SRAM");
        rec_text(ctx, "workload", "10 consecutive ARM calls per game, recorded in Gopher2600 during play; "
                                  "each result checked against Gopher2600 (regs, flags, count, RAM CRC)");
        rec_text(ctx, "figures", "cpi_x1000 = clk_sys cycles per guest instruction x1000 (best of %u runs); "
                                 "mips_x10 = at this clock; mips250_x10 = projected to 250 MHz; "
                                 "need_x10 = MIPS the game needs for a stable frame (0 = unknown)", REPEATS);
    }
    // Which workload: results are comparable only between runs with the same ID.
    rec_value(ctx, "workload_crc32", UNIT_HEX, awl_id());
    rec_value(ctx, "workload_titles", UNIT_COUNT, n);

    uint8_t *mem = bench_ram();
    uint32_t avail = bench_ram_size();
    uint64_t all_cycles = 0, all_instr = 0;
    unsigned measured = 0, with_need = 0, fast_enough = 0;
    char k[REC_LABEL_LEN];

    for (unsigned i = 0; i < n; i++) {
        awl_title_t t;
        awl_title(i, &t);
        uint32_t rom_bytes = (t.rom_size + 3u) & ~3u;
        if (rom_bytes + t.ram_size > avail) {
            key(k, sizeof k, t.name, "skipped");
            rec_text(ctx, k, "needs %lu KB of free SRAM, %lu KB available",
                     (unsigned long)((rom_bytes + t.ram_size) / 1024u), (unsigned long)(avail / 1024u));
            continue;
        }
        uint8_t *rom = mem, *ram = mem + rom_bytes;
        awl_load_rom(&t, rom);

        awl_result_t best = {0}, r;
        for (unsigned rep = 0; rep < REPEATS; rep++) {
            awl_run(&t, rom, ram, timed_run, &r);
            guard_keepalive();
            if (r.bad) {
                key(k, sizeof k, t.name, "mismatch");
                rec_error(ctx, k, (uint32_t)r.first_bad << 8 | r.bad_what, r.bad_expected, r.bad_actual);
                break;
            }
            if (rep == 0 || r.cycles < best.cycles) best = r;
        }
        if (r.bad || best.instructions == 0) continue;

        uint32_t cpi_x1000 = (uint32_t)(best.cycles * 1000u / best.instructions);
        uint32_t mips_here = cpi_x1000 ? (uint32_t)((uint64_t)ctx->clk_khz * 10u / cpi_x1000) : 0;
        uint32_t mips_250 = cpi_x1000 ? (uint32_t)((uint64_t)CART_KHZ * 10u / cpi_x1000) : 0;
        key(k, sizeof k, t.name, "cpi_x1000");
        rec_value(ctx, k, UNIT_COUNT, cpi_x1000);
        all_cycles += best.cycles;
        all_instr += best.instructions;
        measured++;
        if (t.need_mips_x10) {
            with_need++;
            if (mips_250 >= t.need_mips_x10) fast_enough++;
        }
        if (!full) continue;
        key(k, sizeof k, t.name, "format");
        rec_text(ctx, k, "%s, %lu calls, ROM %lu KB (%lu pages present), RAM %lu KB", t.format,
                 (unsigned long)best.calls, (unsigned long)(t.rom_size / 1024u), (unsigned long)t.npages,
                 (unsigned long)(t.ram_size / 1024u));
        key(k, sizeof k, t.name, "instr");
        rec_value(ctx, k, UNIT_COUNT, best.instructions);
        key(k, sizeof k, t.name, "cycles");
        rec_value(ctx, k, UNIT_CYCLES, (int64_t)best.cycles);
        key(k, sizeof k, t.name, "max_call_cycles");
        rec_value(ctx, k, UNIT_CYCLES, best.max_call_cycles);
        key(k, sizeof k, t.name, "mips_x10");
        rec_value(ctx, k, UNIT_COUNT, mips_here);
        key(k, sizeof k, t.name, "mips250_x10");
        rec_value(ctx, k, UNIT_COUNT, mips_250);
        key(k, sizeof k, t.name, "need_x10");
        rec_value(ctx, k, UNIT_COUNT, t.need_mips_x10);
        if (t.need_mips_x10) {
            key(k, sizeof k, t.name, "fast_enough_250");
            rec_value(ctx, k, UNIT_BOOL, mips_250 >= t.need_mips_x10);
        }
    }

    if (measured) {
        uint32_t cpi = (uint32_t)(all_cycles * 1000u / all_instr);
        rec_value(ctx, "all_cpi_x1000", UNIT_COUNT, cpi);
        rec_value(ctx, "all_mips250_x10", UNIT_COUNT, cpi ? (int64_t)((uint64_t)CART_KHZ * 10u / cpi) : 0);
        rec_value(ctx, "games_measured", UNIT_COUNT, measured);
        rec_value(ctx, "games_with_known_need", UNIT_COUNT, with_need);
        rec_value(ctx, "games_fast_enough_250", UNIT_COUNT, fast_enough);
    }
    return measured ? TR_PASS : TR_NA;
}
