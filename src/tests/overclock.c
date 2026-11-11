/*
 * Overclocking explorer (oc.*): PICOBENCH_VARIANT=overclock only (dist/picobench_<chip>_overclock.uf2),
 * never in the normal UF2s or the normal run.
 *
 * Finds the highest clk_sys at which this particular chip still computes, moves data and
 * reads its memories correctly, at each core voltage from the default 1.10 V up to 1.30 V
 * - the regulator's limit while it is locked; this firmware never unlocks it. Everything
 * here is OUTSIDE the chip's specification, true only for this chip, at this temperature,
 * for the few seconds it was tried. It answers "how much headroom is there", not "what
 * clock can a product use".
 *
 * One stage per voltage (950-954), so a hang at one voltage - caught by the watchdog,
 * recorded as TEST_CRASHED - does not stop the others. Each stage starts at the highest
 * clock an earlier stage passed (at least the chip's rated maximum for that voltage) and
 * goes up in OC_STEP_KHZ steps until a step fails, the chip gets hot (OC_TEMP_LIMIT_MC)
 * or OC_MAX_KHZ is reached. The last good step is then run again for OC_CONFIRM_MS.
 *
 * One step:
 *   1. at the default clock: record "try_khz", so that a hang leaves the frequency behind;
 *   2. switch (clocks_set_custom): core voltage first, the flash crossed at a slow
 *      divider, the PLL, then flash SCK <= OC_FLASH_SCK_MAX_KHZ with the boot sample
 *      delay kept in nanoseconds (verified by reading 64 KB back) and PSRAM SCK <= 133
 *      MHz by the SDK's rules. The memories stay within their ratings; only the RP2040 /
 *      RP2350 itself runs outside;
 *   3. for OC_DWELL_MS: core 1 runs an integer kernel non-stop and core 0 runs it too,
 *      both checked against the result computed at the default clock, and in between
 *      core 0 runs the Thumb interpreter on the synthetic load (instruction count and RAM
 *      CRC), an SRAM pattern, a DMA copy, the flash read back through the uncached alias
 *      and a PSRAM pattern;
 *   4. measurements: interpreter speed, flash cache-miss and PSRAM read latency, chip
 *      temperature;
 *   5. back to the default clock (and PSRAM profile), then record. Nothing is written to
 *      the results arena while the clock is raised: a corrupted record would invalidate
 *      the arena's CRC and with it every result of the run.
 * A clock the chip cannot hold usually hangs it rather than giving a wrong result; the
 * watchdog resets it and the next stage goes on. The regulator keeps its voltage through
 * a watchdog reset, so clocks_init() puts it back to the default at every boot.
 *
 * The fastest flash setting (flash_fast.h: quad continuous read with SCK <= 133 MHz, RX
 * delay scanned) is measured by a separate visit to a clock that already passed:
 *   - at the first step of a stage that starts from an earlier stage's best, i.e. a
 *     clock that passed at a lower voltage. Chips that hang before any detected error
 *     never reach a confirmation (the RP2040 in every stage of results_12), and this
 *     gives them a figure too;
 *   - after the confirmation, at the confirmed maximum (if not probed there already),
 *     once the stage's results are recorded.
 * "flash_probe_khz" is recorded before each probe, so a hang in one shows as such.
 *
 * oc.summary (959) collects all stages into one table.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/clocks_ctl.h"
#include "core/flash_clock.h"
#include "core/guard.h"
#include "core/platform.h"
#include "core/stats.h"
#include "core/timing.h"
#include "detect/psram_info.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "results/crc32.h"
#include "results/record.h"
#include "tests/arm/synth_load.h"
#include "tests/flash_fast.h"

#define OC_STEP_KHZ          10000u
#define OC_MAX_KHZ           500000u    // 420 in results_10: RP2040 and RP2350B (Hazard3) reached it
#define OC_TEMP_LIMIT_MC     80000     // stop a stage above this (internal sensor, +-several degC)
#define OC_DWELL_MS          250u
#define OC_CONFIRM_MS        1500u
#define OC_TRANSIT_SCK_KHZ   40000u    // flash SCK while the clock moves
#define OC_FLASH_SCK_MAX_KHZ 75000u    // the fastest flash SCK every board tested has passed
#define OC_PSRAM_SCK_MAX_HZ  (133u * 1000u * 1000u)
#define OC_FLASH_PROBE_SCK_MAX_KHZ 133000u   // the fastest flash SCK verified (433, both RP2350 boards)
#define OC_FLASH_BYTES       (64u * 1024u)
#define OC_PSRAM_OFFSET      (1024u * 1024u)
#define OC_PSRAM_BYTES       (64u * 1024u)
#define OC_KERNEL_N          32768u
#define OC_SEED              0x2545F491u
#define OC_LAT_SAMPLES       64u
#define OC_FIRST_ID          950u
#define OC_LEVELS            5u

static const uint16_t s_level_mv[OC_LEVELS] = {1100, 1150, 1200, 1250, 1300};

enum {
    OC_F_CLOCK = 1u << 0,       // the frequency could not be set
    OC_F_CPU0 = 1u << 2,
    OC_F_CPU1 = 1u << 3,
    OC_F_CORE1 = 1u << 4,       // core 1 did not start or did not stop
    OC_F_SRAM = 1u << 5,
    OC_F_DMA = 1u << 6,
    OC_F_FLASH = 1u << 7,
    OC_F_PSRAM = 1u << 8,
    OC_F_INTERP = 1u << 9,
    OC_F_HOT = 1u << 10,        // not an error: too hot to go on
};

static const char *const s_fail_names[] = {
    "clock", "-", "cpu0", "cpu1", "core1", "sram", "dma", "flash", "psram", "interp", "hot",
};

typedef struct {
    uint32_t khz;           // requested (exactly attainable)
    uint32_t sys_khz;       // frequency counter
    uint32_t fail;          // OC_F_*
    uint32_t flash_div;
    bool flash_slow;        // the <= OC_FLASH_SCK_MAX_KHZ setting did not read back: slow one kept
    uint32_t psram_div;
    int32_t temp_mc;
    uint32_t interp_cycles; // fastest synthetic-load run
    uint32_t miss_p50, miss_max;
    uint32_t ps_p50, ps_max;
    uint32_t c0_runs, c1_runs;
} oc_step_t;

typedef struct {
    uint32_t kernel_ref;
    uint32_t flash_ref;
    uint8_t *rom, *ram, *init;
    thumb_cpu_t cpu;
    int dma_ch;
    bool psram;
} oc_refs_t;

static uint32_t s_tbl0[256], s_tbl1[256];
static volatile uint32_t s_c1_run, s_c1_started, s_c1_done, s_c1_runs, s_c1_bad, s_c1_ref;
static volatile uint32_t s_sink;

// ---- SRAM-resident kernels -------------------------------------------------------------

// Multiplies, shifts, loads and stores over a 1 KB table; deterministic for a given seed.
// No division: on the RP2040 that is a library call, in flash.
static uint32_t __no_inline_not_in_flash_func(oc_kernel)(uint32_t *tbl, uint32_t seed, uint32_t n) {
    for (uint32_t i = 0; i < 256u; i++) {
        tbl[i] = i * 0x01000193u;
        __asm volatile("" ::: "memory");   // not a memset call (in flash)
    }
    uint32_t a = seed, b = 0x9E3779B9u, c = 0;
    for (uint32_t i = 0; i < n; i++) {
        a = a * 1664525u + 1013904223u;
        b ^= a >> 7;
        b = (b << 5) | (b >> 27);
        tbl[a >> 24] += b;
        c += tbl[(b >> 16) & 0xFFu] ^ (a >> 3);
        c = (c << 3) | (c >> 29);
    }
    return a ^ b ^ c;
}

// Core 1: the kernel until told to stop, then parked in SRAM (not back into flash).
static void __no_inline_not_in_flash_func(oc_core1)(void) {
    s_c1_started = 1;
    while (s_c1_run) {
        if (oc_kernel(s_tbl1, OC_SEED, OC_KERNEL_N) != s_c1_ref) s_c1_bad = s_c1_bad + 1u;
        s_c1_runs = s_c1_runs + 1u;
    }
    s_c1_done = 1;
    while (true) __wfe();
}

static void __no_inline_not_in_flash_func(oc_psram_latency)(uint32_t *out, uint32_t n) {
    const volatile uint32_t *p = (const volatile uint32_t *)(PSRAM_NOCACHE_BASE + OC_PSRAM_OFFSET);
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < n; i++) {
        const volatile uint32_t *q = p + i * 16u;
        __asm volatile("" : "+r"(q));
        uint32_t t0 = timing_now();
        uint32_t v = *q;
        uint32_t t1 = timing_now();
        out[i] = timing_net(t0, t1);
        s_sink += v;
    }
    restore_interrupts(irq);
}

// ---- checks (run at the raised clock) ----------------------------------------------------

static uint32_t xorshift(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static bool oc_sram_check(uint32_t seed) {
    uint32_t x = seed;
    for (uint32_t i = 0; i < BENCH_SAMPLES_MAX; i++) {
        x = xorshift(x);
        g_bench_samples[i] = x ^ i;
    }
    x = seed;
    for (uint32_t i = 0; i < BENCH_SAMPLES_MAX; i++) {
        x = xorshift(x);
        if (g_bench_samples[i] != (x ^ i)) return false;
    }
    return true;
}

// Copies the first half of the sample buffer over the second half and compares.
static bool oc_dma_check(int ch) {
    const uint32_t half = BENCH_SAMPLES_MAX / 2u;
    dma_channel_config c = dma_channel_get_default_config((uint)ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    dma_channel_configure((uint)ch, &c, g_bench_samples + half, g_bench_samples, half, true);
    dma_channel_wait_for_finish_blocking((uint)ch);
    for (uint32_t i = 0; i < half; i++) {
        if (g_bench_samples[half + i] != g_bench_samples[i]) return false;
    }
    return true;
}

static bool oc_psram_check(uint32_t seed) {
    volatile uint32_t *p = (volatile uint32_t *)(PSRAM_NOCACHE_BASE + OC_PSRAM_OFFSET);
    uint32_t x = seed;
    for (uint32_t i = 0; i < OC_PSRAM_BYTES / 4u; i++) {
        x = xorshift(x);
        p[i] = x;
    }
    x = seed;
    for (uint32_t i = 0; i < OC_PSRAM_BYTES / 4u; i++) {
        x = xorshift(x);
        if (p[i] != x) return false;
    }
    return true;
}

// One run of the synthetic load. True if it ended with the right instruction count and
// RAM contents; *cycles is how long the interpreter took.
static bool oc_interp(const oc_refs_t *rf, uint32_t *cycles) {
    memcpy(rf->ram, rf->init, SYNTH_RAM_SIZE);
    thumb_cpu_t run = rf->cpu;
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = timing_now();
    uint32_t r = thumb_run(&run, 10u * 1000u * 1000u);
    uint32_t t1 = timing_now();
    restore_interrupts(irq);
    *cycles = timing_net(t0, t1);
    return r == THUMB_ENDED && run.count == SYNTH_INSTRUCTIONS &&
           crc32_update(0, rf->ram, SYNTH_RAM_SIZE) == SYNTH_RAM_CRC;
}

static int32_t oc_temp_mc(void) {
#if PICO_RP2040
    adc_select_input(4);
#else
    adc_select_input(platform_chip() == CHIP_RP2350B ? 8u : 4u);   // see inventory.c
#endif
    (void)adc_read();
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += adc_read();
    int32_t uv = (int32_t)((uint64_t)(sum / 16u) * 3300000u / 4096u);
    return 27000 - (uv - 706000) * 1000 / 1721;
}

// Step 3: both cores busy for dwell_ms, core 0 also cycling through the other checks.
static void oc_dwell(uint32_t dwell_ms, const oc_refs_t *rf, oc_step_t *st) {
    s_c1_run = 1;
    s_c1_started = 0;
    s_c1_done = 0;
    s_c1_runs = 0;
    s_c1_bad = 0;
    s_c1_ref = rf->kernel_ref;
    multicore_reset_core1();
    multicore_launch_core1(oc_core1);
    uint64_t until = time_us_64() + 100000u;
    while (!s_c1_started && time_us_64() < until) {}
    if (!s_c1_started) st->fail |= OC_F_CORE1;

    uint64_t end = time_us_64() + (uint64_t)dwell_ms * 1000u;
    uint32_t round = 0;
    st->interp_cycles = UINT32_MAX;
    while (!st->fail && (time_us_64() < end || st->interp_cycles == UINT32_MAX)) {
        if (oc_kernel(s_tbl0, OC_SEED, OC_KERNEL_N) != rf->kernel_ref) st->fail |= OC_F_CPU0;
        st->c0_runs++;
        uint32_t seed = OC_SEED ^ (round * 0x9E3779B9u);
        switch (round++ % 4u) {
        case 0: {
            uint32_t cyc;
            if (!oc_interp(rf, &cyc)) st->fail |= OC_F_INTERP;
            else if (cyc < st->interp_cycles) st->interp_cycles = cyc;
            break;
        }
        case 1:
            if (!oc_sram_check(seed)) st->fail |= OC_F_SRAM;
            else if (!oc_dma_check(rf->dma_ch)) st->fail |= OC_F_DMA;
            break;
        case 2:
            if (flash_clock_checksum(OC_FLASH_BYTES) != rf->flash_ref) st->fail |= OC_F_FLASH;
            break;
        default:
            if (rf->psram && !oc_psram_check(seed)) st->fail |= OC_F_PSRAM;
            break;
        }
    }
    if (st->interp_cycles == UINT32_MAX) st->interp_cycles = 0;

    s_c1_run = 0;
    until = time_us_64() + 200000u;
    while (!s_c1_done && time_us_64() < until) {}
    if (!s_c1_done) st->fail |= OC_F_CORE1;
    if (s_c1_bad) st->fail |= OC_F_CPU1;
    st->c1_runs = s_c1_runs;
    multicore_reset_core1();
}

// Step 4, still at the raised clock.
static void oc_measure(const oc_refs_t *rf, oc_step_t *st) {
    stats_t s;
    xip_cache_clean_all();      // nothing cached from PSRAM may be lost by the invalidates
    bench_cold_reads((const uint8_t *)(XIP_BASE + 32u * 1024u), 64u, OC_LAT_SAMPLES, g_bench_samples);
    stats_from_samples(g_bench_samples, OC_LAT_SAMPLES, &s);
    st->miss_p50 = s.p50;
    st->miss_max = s.max;
    if (rf->psram) {
        oc_psram_latency(g_bench_samples, OC_LAT_SAMPLES);
        stats_from_samples(g_bench_samples, OC_LAT_SAMPLES, &s);
        st->ps_p50 = s.p50;
        st->ps_max = s.max;
    }
}

static void oc_back_to_default(const oc_refs_t *rf) {
    (void)rf;
    clocks_set_op(clocks_default_op());
    if (g_psram.present) psram_reapply();   // also after the flash probe, which rewrote CS1
}

// Steps 2-5. Returns at the default clock.
static void oc_step(uint32_t khz, uint32_t mv, uint32_t dwell_ms, const oc_refs_t *rf, oc_step_t *st) {
    memset(st, 0, sizeof(*st));
    st->khz = khz;
    flash_clock_t fc;
    if (!clocks_set_custom(khz, mv, OC_TRANSIT_SCK_KHZ, &fc)) {
        st->fail = OC_F_CLOCK;
        return;
    }
    flash_clock_t fast;
    flash_clock_for(khz, OC_FLASH_SCK_MAX_KHZ, &fast);
    // If the fast setting does not read back, the step goes on with the slow one (the
    // CPU result still counts) and says so: that is the flash interface's own limit.
    if (fast.div < fc.div) {
        if (flash_clock_try(&fast, rf->flash_ref, OC_FLASH_BYTES, 4)) fc = fast;
        else st->flash_slow = true;
    }
    st->flash_div = fc.div;
    if (rf->psram) st->psram_div = psram_retime_direct(khz * 1000u, OC_PSRAM_SCK_MAX_HZ);
    st->sys_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    if (!st->fail) {
        oc_dwell(dwell_ms, rf, st);
        if (!st->fail) oc_measure(rf, st);
    }
    st->temp_mc = oc_temp_mc();
    if (st->temp_mc > OC_TEMP_LIMIT_MC) st->fail |= OC_F_HOT;
    oc_back_to_default(rf);
}

// The flash probe: back to khz, the fastest flash setting measured there, back again.
static void oc_flash_probe(uint32_t khz, uint32_t mv, const oc_refs_t *rf, flash_fast_t *ff) {
    memset(ff, 0, sizeof(*ff));
    flash_clock_t fc;
    if (!clocks_set_custom(khz, mv, OC_TRANSIT_SCK_KHZ, &fc)) {
        ff->why = "clock not attainable";
        return;
    }
    flash_clock_t fast;
    flash_clock_for(khz, OC_FLASH_SCK_MAX_KHZ, &fast);
    if (fast.div < fc.div) flash_clock_try(&fast, rf->flash_ref, OC_FLASH_BYTES, 4);
    flash_fast_probe(khz, OC_FLASH_PROBE_SCK_MAX_KHZ, ff);
    oc_back_to_default(rf);
}

// ---- recording -----------------------------------------------------------------------------

static void fail_text(uint32_t fail, char *out, size_t len) {
    size_t n = 0;
    out[0] = '\0';
    for (unsigned b = 0; b < sizeof(s_fail_names) / sizeof(s_fail_names[0]) && n < len; b++) {
        if (fail & (1u << b)) n += (size_t)snprintf(out + n, len - n, "%s%s", n ? "," : "", s_fail_names[b]);
    }
}

static uint32_t ns_of(uint32_t cycles, uint32_t khz) {
    return khz ? (uint32_t)((uint64_t)cycles * 1000000u / khz) : 0;
}

static uint32_t mips_x100(uint32_t cycles, uint32_t khz) {
    return cycles ? (uint32_t)((uint64_t)SYNTH_INSTRUCTIONS * khz / cycles / 10u) : 0;
}

static void oc_record_step(test_ctx_t *ctx, const char *tag, const oc_step_t *st, uint32_t mv) {
    char key[REC_LABEL_LEN], fails[64];
    fail_text(st->fail, fails, sizeof fails);
    int32_t t = st->temp_mc;
    snprintf(key, sizeof key, "%s%luMHz", tag, (unsigned long)(st->khz / 1000u));
    if (st->fail & OC_F_CLOCK) {
        rec_text(ctx, key, "not attainable from the crystal");
        return;
    }
    rec_text(ctx, key, "%s%s | clk_sys %lu kHz, %lu mV, %ld.%01ld C | flash div %lu (SCK %lu kHz%s), PSRAM div %lu | "
                       "kernel runs %lu+%lu",
             st->fail ? "FAIL " : "ok", fails, (unsigned long)st->sys_khz, (unsigned long)mv, (long)(t / 1000),
             (long)((t < 0 ? -t : t) % 1000 / 100), (unsigned long)st->flash_div,
             (unsigned long)(st->flash_div ? st->khz / st->flash_div : 0), st->flash_slow ? ", FAST ONE FAILED" : "",
             (unsigned long)st->psram_div, (unsigned long)st->c0_runs, (unsigned long)st->c1_runs);
    if (st->fail) return;
    uint32_t m = mips_x100(st->interp_cycles, st->khz);
    snprintf(key, sizeof key, "%s%luMHz_perf", tag, (unsigned long)(st->khz / 1000u));
    rec_text(ctx, key, "interpreter %lu.%02lu MIPS | flash miss p50 %lu max %lu cyc = %lu ns | "
                       "PSRAM read p50 %lu max %lu cyc = %lu ns",
             (unsigned long)(m / 100u), (unsigned long)(m % 100u), (unsigned long)st->miss_p50,
             (unsigned long)st->miss_max, (unsigned long)ns_of(st->miss_p50, st->khz),
             (unsigned long)st->ps_p50, (unsigned long)st->ps_max, (unsigned long)ns_of(st->ps_p50, st->khz));
}

static void oc_record_flash(test_ctx_t *ctx, uint32_t khz, const flash_fast_t *ff) {
    if (!ff->tried) {
        rec_text(ctx, "flash_fast", "not tried: %s", ff->why ? ff->why : "?");
        return;
    }
    char list[24];
    size_t n = 0;
    list[0] = '\0';
    for (unsigned i = 0; i < 8 && n < sizeof list; i++) {
        if (ff->rx_mask & (1u << i)) n += (size_t)snprintf(list + n, sizeof list - n, "%s%u", n ? "," : "", i);
    }
    if (!ff->rx_mask) snprintf(list, sizeof list, "none");
    if (!ff->verified) {
        rec_text(ctx, "flash_fast", "quad continuous, div %u (SCK %lu kHz) at %lu kHz: no RX delay passed "
                                    "(scan: %s)", ff->div, (unsigned long)ff->sck_khz, (unsigned long)khz, list);
        return;
    }
    rec_text(ctx, "flash_fast", "quad continuous, div %u (SCK %lu kHz) at %lu kHz, RX delay %u (working: %s): "
                                "miss p50 %lu max %lu cyc = %lu ns",
             ff->div, (unsigned long)ff->sck_khz, (unsigned long)khz, ff->rx, list, (unsigned long)ff->miss_p50,
             (unsigned long)ff->miss_max, (unsigned long)ns_of(ff->miss_p50, khz));
    rec_value(ctx, "flash_fast_miss_ns", UNIT_NS, ns_of(ff->miss_p50, khz));
    rec_value(ctx, "flash_fast_at", UNIT_KHZ, khz);
    rec_value(ctx, "flash_fast_sck", UNIT_KHZ, ff->sck_khz);
    if (ff->read4k) rec_value(ctx, "flash_fast_rate", UNIT_KBPS, (int64_t)((uint64_t)4096u * khz / ff->read4k));
}

// The highest "ok_khz" any stage of this run has recorded so far.
static uint32_t best_ok_so_far(void) {
    uint32_t best = 0;
    for (const rec_hdr_t *h = record_next(NULL); h; h = record_next(h)) {
        if (h->type != REC_VALUE || h->test_id < OC_FIRST_ID || h->test_id >= OC_FIRST_ID + OC_LEVELS) continue;
        const rec_value_t *v = (const rec_value_t *)h;
        if (strcmp(v->key, "ok_khz") == 0 && (uint32_t)v->value > best) best = (uint32_t)v->value;
    }
    return best;
}

static uint32_t rated_khz(uint32_t mv) {
#if PICO_RP2040
    return mv >= 1150u ? 200000u : 133000u;
#else
    (void)mv;
    return 150000u;
#endif
}

// ---- the stages ------------------------------------------------------------------------------

static bool oc_prepare(test_ctx_t *ctx, oc_refs_t *rf) {
    memset(rf, 0, sizeof(*rf));
    const uint32_t need = SYNTH_ROM_SIZE + 2u * SYNTH_RAM_SIZE;
    if (bench_ram_size() < need) {
        rec_text(ctx, "skipped", "needs %lu KB of free SRAM, %lu KB available", (unsigned long)(need / 1024u),
                 (unsigned long)(bench_ram_size() / 1024u));
        return false;
    }
    rf->rom = bench_ram();
    rf->ram = rf->rom + SYNTH_ROM_SIZE;
    rf->init = rf->ram + SYNTH_RAM_SIZE;
    synth_load_stage(rf->rom, rf->ram, &rf->cpu);
    memcpy(rf->init, rf->ram, SYNTH_RAM_SIZE);
    rf->kernel_ref = oc_kernel(s_tbl0, OC_SEED, OC_KERNEL_N);
    rf->flash_ref = flash_clock_checksum(OC_FLASH_BYTES);
    flash_fast_prepare();
    rf->psram = g_psram.present && g_psram.initialised && g_psram.size >= OC_PSRAM_OFFSET + OC_PSRAM_BYTES;
    if (rf->psram && !psram_reapply()) rf->psram = false;
    rf->dma_ch = dma_claim_unused_channel(false);
    if (rf->dma_ch < 0) {
        rec_text(ctx, "skipped", "no free DMA channel");
        return false;
    }
    uint32_t cyc;
    if (!oc_interp(rf, &cyc)) {
        rec_error(ctx, "synth_reference", 0, SYNTH_INSTRUCTIONS, 0);
        dma_channel_unclaim((uint)rf->dma_ch);
        return false;
    }
    if (!oc_sram_check(OC_SEED) || !oc_dma_check(rf->dma_ch) || (rf->psram && !oc_psram_check(OC_SEED))) {
        rec_error(ctx, "reference_checks", 0, 1, 0);
        dma_channel_unclaim((uint)rf->dma_ch);
        return false;
    }
    adc_init();
    adc_set_temp_sensor_enabled(true);
    uint32_t m = mips_x100(cyc, ctx->clk_khz);
    rec_text(ctx, "reference", "at %lu kHz: interpreter %lu.%02lu MIPS (%lu cycles for %u instructions), "
                               "kernel 0x%08lx, flash 0x%08lx%s",
             (unsigned long)ctx->clk_khz, (unsigned long)(m / 100u), (unsigned long)(m % 100u),
             (unsigned long)cyc, SYNTH_INSTRUCTIONS, (unsigned long)rf->kernel_ref, (unsigned long)rf->flash_ref,
             rf->psram ? ", PSRAM checked" : ", no PSRAM");
    return true;
}

test_result_t test_oc_explore(test_ctx_t *ctx) {
    const unsigned level = ctx->test->id - OC_FIRST_ID;
    if (level >= OC_LEVELS) return TR_NA;
    const uint32_t mv = s_level_mv[level];
    rec_text(ctx, "warning", "OUTSIDE SPECIFICATION: results hold for this chip, now, for seconds - not a "
                             "clock to use. Core supply %lu mV (regulator limit 1300 mV, never unlocked).",
             (unsigned long)mv);
    oc_refs_t rf;
    if (!oc_prepare(ctx, &rf)) return TR_SKIPPED;

    uint32_t start = rated_khz(mv);
    uint32_t prev = best_ok_so_far();
    const bool early_probe = prev > start;   // starting from a clock an earlier stage passed
    if (prev > start) start = prev;
    rec_value(ctx, "start_khz", UNIT_KHZ, start);
    uint32_t probed_khz = 0;

    uint32_t last_ok = 0, fail_khz = 0, fail_bits = 0;
    oc_step_t st, ok_st;
    memset(&ok_st, 0, sizeof ok_st);
    for (uint32_t target = start; target <= OC_MAX_KHZ;
         target = (target / OC_STEP_KHZ + 1u) * OC_STEP_KHZ) {
        uint32_t khz = clocks_attainable_khz(target, 4000u);
        if (!khz || khz <= last_ok) continue;
        guard_keepalive();
        rec_value(ctx, "try_khz", UNIT_KHZ, khz);
        oc_step(khz, mv, OC_DWELL_MS, &rf, &st);
        oc_record_step(ctx, "", &st, mv);
        if (st.fail & OC_F_CLOCK) continue;
        if (st.fail) {
            fail_khz = khz;
            fail_bits = st.fail;
            break;
        }
        rec_value(ctx, "ok_khz", UNIT_KHZ, khz);
        last_ok = khz;
        ok_st = st;
        if (early_probe && !probed_khz) {
            guard_keepalive();
            rec_value(ctx, "flash_probe_khz", UNIT_KHZ, khz);
            flash_fast_t ff;
            oc_flash_probe(khz, mv, &rf, &ff);
            oc_record_flash(ctx, khz, &ff);
            probed_khz = khz;
        }
    }

    char fails[64];
    bool confirmed = false;
    if (last_ok) {
        guard_keepalive();
        rec_value(ctx, "confirm_khz", UNIT_KHZ, last_ok);
        oc_step(last_ok, mv, OC_CONFIRM_MS, &rf, &st);
        oc_record_step(ctx, "confirm_", &st, mv);
        confirmed = st.fail == 0;
    }
    dma_channel_unclaim((uint)rf.dma_ch);

    rec_value(ctx, "max_stable_khz", UNIT_KHZ, last_ok);
    rec_value(ctx, "confirmed", UNIT_BOOL, confirmed);
    if (fail_khz) {
        fail_text(fail_bits, fails, sizeof fails);
        rec_value(ctx, "first_fail_khz", UNIT_KHZ, fail_khz);
        rec_text(ctx, "stopped", "%s at %lu kHz: %s", (fail_bits & ~OC_F_HOT) ? "failed" : "too hot",
                 (unsigned long)fail_khz, fails);
    } else {
        rec_text(ctx, "stopped", "reached the explorer's limit of %lu kHz without a failure",
                 (unsigned long)OC_MAX_KHZ);
    }
    if (last_ok) {
        rec_value(ctx, "interp_mips_x100_at_max", UNIT_COUNT, mips_x100(ok_st.interp_cycles, last_ok));
        rec_value(ctx, "flash_miss_ns_at_max", UNIT_NS, ns_of(ok_st.miss_p50, last_ok));
        if (rf.psram) rec_value(ctx, "psram_read_ns_at_max", UNIT_NS, ns_of(ok_st.ps_p50, last_ok));
        rec_value(ctx, "temp_at_max", UNIT_MILLI_C, ok_st.temp_mc);
    }
    // Last, so that a hang here cannot hide the results above.
    if (last_ok && confirmed && last_ok != probed_khz) {
        guard_keepalive();
        rec_value(ctx, "flash_probe_khz", UNIT_KHZ, last_ok);
        flash_fast_t ff;
        oc_flash_probe(last_ok, mv, &rf, &ff);
        oc_record_flash(ctx, last_ok, &ff);
    }
    // A failing step is the answer, not a test failure; only broken reference checks are.
    return TR_PASS;
}

// ---- summary -----------------------------------------------------------------------------------

typedef struct {
    bool ran;
    uint8_t result;
    uint32_t last_try, last_ok, max_stable, first_fail, confirm, mips_x100;
    uint32_t flash_ns, flash_at;   // the last flash probe that measured something
    uint32_t probe_pending;        // a probe was started at this clock and nothing followed it
    bool confirm_seen, confirmed, have_max;
    char stopped[48];
} oc_level_t;

test_result_t test_oc_summary(test_ctx_t *ctx) {
    oc_level_t lv[OC_LEVELS];
    memset(lv, 0, sizeof lv);
    for (const rec_hdr_t *h = record_next(NULL); h; h = record_next(h)) {
        if (h->test_id < OC_FIRST_ID || h->test_id >= OC_FIRST_ID + OC_LEVELS) continue;
        oc_level_t *l = &lv[h->test_id - OC_FIRST_ID];
        if (h->type == REC_TEST_END) {
            l->ran = true;
            l->result = ((const rec_test_end_t *)h)->result;
        } else if (h->type == REC_VALUE) {
            const rec_value_t *v = (const rec_value_t *)h;
            uint32_t x = (uint32_t)v->value;
            if (!strcmp(v->key, "try_khz")) { l->last_try = x; l->probe_pending = 0; }
            else if (!strcmp(v->key, "ok_khz")) l->last_ok = x;
            else if (!strcmp(v->key, "confirm_khz")) { l->confirm = x; l->confirm_seen = true; l->probe_pending = 0; }
            else if (!strcmp(v->key, "max_stable_khz")) { l->max_stable = x; l->have_max = true; }
            else if (!strcmp(v->key, "flash_probe_khz")) l->probe_pending = x;
            else if (!strcmp(v->key, "flash_fast_miss_ns")) l->flash_ns = x;
            else if (!strcmp(v->key, "flash_fast_at")) l->flash_at = x;
            else if (!strcmp(v->key, "first_fail_khz")) l->first_fail = x;
            else if (!strcmp(v->key, "confirmed")) l->confirmed = x != 0;
            else if (!strcmp(v->key, "interp_mips_x100_at_max")) l->mips_x100 = x;
        } else if (h->type == REC_TEXT) {
            const rec_text_t *t = (const rec_text_t *)h;
            if (!strcmp(t->key, "stopped")) snprintf(l->stopped, sizeof l->stopped, "%s", t->value);
            else if (!strcmp(t->key, "flash_fast")) l->probe_pending = 0;
        }
    }

    uint32_t best = 0, best_mv = 0;
    for (unsigned i = 0; i < OC_LEVELS; i++) {
        const oc_level_t *l = &lv[i];
        char key[REC_LABEL_LEN];
        snprintf(key, sizeof key, "%umV", s_level_mv[i]);
        if (!l->ran) {
            rec_text(ctx, key, "not run");
            continue;
        }
        char flash[64] = "";
        if (l->flash_ns) {
            snprintf(flash, sizeof flash, "; fastest flash miss %lu ns at %lu MHz", (unsigned long)l->flash_ns,
                     (unsigned long)(l->flash_at / 1000u));
        }
        const bool probe_hung = l->result == TR_CRASHED && l->probe_pending;
        if (probe_hung && !l->have_max) {
            rec_text(ctx, key, "HUNG in the flash probe at %lu kHz (watchdog reset); last good step %lu kHz%s",
                     (unsigned long)l->probe_pending, (unsigned long)l->last_ok, flash);
            continue;
        }
        if (l->result == TR_CRASHED && !probe_hung) {
            uint32_t at = l->confirm_seen ? l->confirm : l->last_try;
            if (l->last_ok && !l->confirm_seen) {
                rec_text(ctx, key, "HUNG at %lu kHz (watchdog reset); last good step %lu kHz, not confirmed%s",
                         (unsigned long)at, (unsigned long)l->last_ok, flash);
            } else {
                rec_text(ctx, key, "HUNG at %lu kHz (watchdog reset)%s%s", (unsigned long)at,
                         l->confirm_seen ? " during the confirmation run" : ", no step passed at this voltage",
                         flash);
            }
            continue;
        }
        if (l->result != TR_PASS && !probe_hung) {
            rec_text(ctx, key, "not completed (%s)", l->result == TR_SKIPPED ? "skipped" : "failed");
            continue;
        }
        if (!l->max_stable) {
            rec_text(ctx, key, "no step passed; %s", l->stopped);
            continue;
        }
        if (probe_hung) snprintf(flash, sizeof flash, "; HUNG in the flash probe");
        rec_text(ctx, key, "max stable %lu kHz%s, interpreter %lu.%02lu MIPS%s; %s",
                 (unsigned long)l->max_stable, l->confirmed ? " (confirmed)" : " (NOT confirmed)",
                 (unsigned long)(l->mips_x100 / 100u), (unsigned long)(l->mips_x100 % 100u), flash, l->stopped);
        snprintf(key, sizeof key, "max_khz_%umV", s_level_mv[i]);
        rec_value(ctx, key, UNIT_KHZ, l->max_stable);
        if (l->confirmed && l->max_stable > best) {
            best = l->max_stable;
            best_mv = s_level_mv[i];
        }
    }
    rec_value(ctx, "best_confirmed_khz", UNIT_KHZ, best);
    rec_value(ctx, "best_confirmed_at", UNIT_MV, best_mv);
    rec_text(ctx, "note", "each voltage: %u MHz steps, %u ms per step on both cores, the last good step "
                          "again for %u ms; memories kept within their ratings (flash SCK <= %u MHz, "
                          "PSRAM <= 133 MHz). Leave a margin: these are limits, not operating points.",
             OC_STEP_KHZ / 1000u, OC_DWELL_MS, OC_CONFIRM_MS, OC_FLASH_SCK_MAX_KHZ / 1000u);
    return TR_PASS;
}
