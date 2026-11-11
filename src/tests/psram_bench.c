/*
 * PSRAM performance (RP2350 with a PSRAM chip fitted).
 *
 * Two aliases matter and both are measured: 0x11000000 goes through the 16 KB XIP
 * cache (shared with flash), 0x15000000 always reaches the chip. Cached numbers say
 * what a small working set costs, uncached numbers say what the chip really delivers.
 *
 * The PSRAM clock is clk_sys / divisor, so the timing has to be recomputed whenever
 * clk_sys changes - this test does that on entry and reports the divisor it ended up
 * with, which is why it can be swept across operating points at all.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "detect/psram_info.h"
#include "results/record.h"

#if PICO_RP2040

test_result_t test_psram_bench(test_ctx_t *ctx) { (void)ctx; return TR_NA; }
test_result_t test_psram_copy(test_ctx_t *ctx) { (void)ctx; return TR_NA; }
test_result_t test_psram_timing_sweep(test_ctx_t *ctx) { (void)ctx; return TR_NA; }

#else

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/psram.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"

#define SAMPLES     128u
#define BLOCK_BYTES 4096u
#define BIG_BYTES   65536u
#define FIELD(v, NAME) (((v) & NAME##_BITS) >> NAME##_LSB)

static volatile uint32_t s_sink;

static int64_t kbps(uint32_t bytes, uint32_t cycles, uint32_t clk_khz) {
    return cycles ? (int64_t)((uint64_t)bytes * clk_khz / cycles) : 0;
}

// ---- Access kernels (in SRAM) -----------------------------------------------------------

static uint32_t __no_inline_not_in_flash_func(rd32)(void *base, uint32_t bytes) {
    const volatile uint32_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static uint32_t __no_inline_not_in_flash_func(rd8)(void *base, uint32_t bytes) {
    const volatile uint8_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes; i += 8) {
        acc += (uint32_t)p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static uint32_t __no_inline_not_in_flash_func(wr32)(void *base, uint32_t bytes) {
    volatile uint32_t *p = base;
    uint32_t v = 0x5A5A1234u;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
    return v;
}

static uint32_t __no_inline_not_in_flash_func(wr8)(void *base, uint32_t bytes) {
    volatile uint8_t *p = base;
    uint8_t v = 0x5A;
    for (uint32_t i = 0; i < bytes; i += 8) {
        p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
    return v;
}

// A single read: the unit of latency.
static uint32_t __no_inline_not_in_flash_func(rd_one)(void *base, uint32_t unused) {
    (void)unused;
    return *(const volatile uint32_t *)base;
}

// Dependent reads: each address comes from the previous load.
static uint32_t s_chase_steps;
static uint32_t __no_inline_not_in_flash_func(chase)(void *base, uint32_t start) {
    const volatile uint32_t *p = base;
    uint32_t idx = start;
    for (uint32_t i = 0; i < s_chase_steps; i += 8) {
        idx = p[idx]; idx = p[idx]; idx = p[idx]; idx = p[idx];
        idx = p[idx]; idx = p[idx]; idx = p[idx]; idx = p[idx];
    }
    return idx;
}

typedef uint32_t (*access_fn)(void *base, uint32_t arg);

// The sampling loop runs from SRAM. A loop in flash shares the 16 KB XIP cache with the
// PSRAM data it measures, so the cached figures depended on where the linker put it:
// chase_cached_256k went from 2.3-2.8 to 3.3-4.1 cycles per load between two builds of
// the same code (results_8 -> results_9). Nothing in here fetches from flash.
static void __no_inline_not_in_flash_func(sample_loop)(access_fn fn, void *base, uint32_t arg,
                                                       uint32_t samples) {
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < samples; i++) {
        void *b = base;
        uint32_t a = arg;
        __asm volatile("" : "+r"(b), "+r"(a));
        uint32_t t0 = timing_now();
        uint32_t r = fn(b, a);
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    restore_interrupts(irq);
}

static void sample_access(access_fn fn, void *base, uint32_t arg, uint32_t samples, stats_t *out) {
    access_fn volatile vfn = fn;   // opaque: keeps the call inside the timed region
    if (samples > BENCH_SAMPLES_MAX) samples = BENCH_SAMPLES_MAX;
    s_sink += vfn(base, arg);
    sample_loop(vfn, base, arg, samples);
    stats_from_samples(g_bench_samples, samples, out);
}

static void record_access(test_ctx_t *ctx, const char *label, access_fn fn, void *base,
                          uint32_t bytes, uint32_t clk_khz) {
    stats_t s;
    uint32_t samples = bytes >= BIG_BYTES ? 16u : SAMPLES;
    sample_access(fn, base, bytes, samples, &s);
    rec_stats(ctx, label, UNIT_CYCLES, bytes, &s);
    char key[REC_LABEL_LEN + 24];
    snprintf(key, sizeof(key), "%.*s_rate", (int)(sizeof(key) - 6), label);
    rec_value(ctx, key, UNIT_KBPS, kbps(bytes, s.p50, clk_khz));
    guard_keepalive();
}

// Re-apply the SDK default profile for the current clk_sys and report what it produced.
static bool psram_retime(test_ctx_t *ctx) {
    if (!psram_reapply()) {
        rec_error(ctx, "psram_retime", 0, 1, 0);
        return false;
    }
    uint32_t t = qmi_hw->m[1].timing;
    uint32_t div = FIELD(t, QMI_M1_TIMING_CLKDIV);
    rec_text(ctx, "timing", "divisor %lu -> SCK %lu kHz, rxdelay %lu, max_select %lu, min_deselect %lu",
             (unsigned long)div, (unsigned long)(div ? ctx->clk_khz / div : 0),
             (unsigned long)FIELD(t, QMI_M1_TIMING_RXDELAY),
             (unsigned long)FIELD(t, QMI_M1_TIMING_MAX_SELECT),
             (unsigned long)FIELD(t, QMI_M1_TIMING_MIN_DESELECT));
    return true;
}

// ---- Throughput and latency ----------------------------------------------------------------

test_result_t test_psram_bench(test_ctx_t *ctx) {
    if (!g_psram.present || !g_psram.initialised) return TR_NA;
    if (!psram_retime(ctx)) return TR_FAIL;

    void *cached = (void *)PSRAM_CACHED_BASE;
    void *uncached = (void *)PSRAM_NOCACHE_BASE;

    record_access(ctx, "uncached_read32_4k", rd32, uncached, BLOCK_BYTES, ctx->clk_khz);
    record_access(ctx, "uncached_read32_64k", rd32, uncached, BIG_BYTES, ctx->clk_khz);
    record_access(ctx, "uncached_read8_4k", rd8, uncached, BLOCK_BYTES, ctx->clk_khz);
    record_access(ctx, "uncached_write32_4k", wr32, uncached, BLOCK_BYTES, ctx->clk_khz);
    record_access(ctx, "uncached_write32_64k", wr32, uncached, BIG_BYTES, ctx->clk_khz);
    record_access(ctx, "uncached_write8_4k", wr8, uncached, BLOCK_BYTES, ctx->clk_khz);

    // Cached: 4 KB fits the XIP cache, 64 KB does not.
    record_access(ctx, "cached_read32_4k", rd32, cached, BLOCK_BYTES, ctx->clk_khz);
    record_access(ctx, "cached_read32_64k", rd32, cached, BIG_BYTES, ctx->clk_khz);

    // Single-access latency: cache hit, cold miss, and straight to the chip.
    stats_t s;
    sample_access(rd_one, cached, 0, SAMPLES, &s);
    rec_stats(ctx, "read_hit", UNIT_CYCLES, 1, &s);
    // Whole sample in SRAM: with the loop in flash, the instruction fetches after each
    // invalidate went through the same QMI and were counted too (124 cycles on M33 but
    // 173 on Hazard3 in results_6, depending on the code layout).
    xip_cache_clean_all();      // write back cached PSRAM data before invalidating
    bench_cold_reads((const uint8_t *)cached, 64u, SAMPLES, g_bench_samples);
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "read_miss_cold", UNIT_CYCLES, 1, &s);
    rec_value(ctx, "read_miss_worst_cycles", UNIT_CYCLES, s.max);
    rec_text(ctx, "sampling", "every sampling loop runs from SRAM (since 2026-11-07); cached "
                              "figures from earlier builds depended on the code layout");
    rec_text(ctx, "miss_method", "whole sample in SRAM (invalidate, read); figures before "
                                 "2026-11 included instruction fetches from flash");
    guard_keepalive();

    sample_access(rd_one, uncached, 0, SAMPLES, &s);
    rec_stats(ctx, "read_uncached_single", UNIT_CYCLES, 1, &s);

    // Dependent-load latency over a working set that fits the cache (8 KB) and one 16x
    // larger than it (256 KB). Every sample walks the whole working set - one cycle
    // through all its words (Sattolo) - so the cached figure is the steady state of that
    // working set. Until 2026-11-07 every walk was 256 steps: it touched only 2 KB, and
    // the "256 KB" figure moved between builds (2.3-4.3 cycles per load) with a handful of
    // cache set conflicts among those 256 lines. Hence the new keys (chase_all_*).
    static const uint32_t sets[] = {8192, 262144};
    for (unsigned i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        if (sets[i] > g_psram.size) break;
        volatile uint32_t *p = uncached;
        uint32_t words = sets[i] / 4u;
        uint32_t seed = 0xBEEF0001u + i;
        xip_cache_clean_all();      // no dirty line may later land on top of the cycle
        for (uint32_t k = 0; k < words; k++) p[k] = k;
        for (uint32_t k = words - 1; k > 0; k--) {     // Sattolo: one cycle over all entries
            uint32_t j = bench_rand(&seed) % k;
            uint32_t t = p[k]; p[k] = p[j]; p[j] = t;
        }
        // The cycle was written through the uncached alias: drop whatever the cache still
        // holds for these addresses. The first full walk (results_14) read a stale line -
        // an earlier test's 0x5A5A1234 - as an index, faulted far outside the PSRAM and
        // hung at every operating point.
        xip_cache_invalidate_all();
        // Walk it once, masked, through the cached alias: one cycle over every word back to
        // 0, or the measurement below would follow a broken chain.
        const volatile uint32_t *pc = (const volatile uint32_t *)cached;
        uint32_t idx = 0, steps = 0;
        do {
            idx = pc[idx] & (words - 1u);
            steps++;
        } while (idx != 0 && steps <= words);
        guard_keepalive();
        if (steps != words) {
            rec_error(ctx, "chase_cycle", sets[i], words, steps);
            continue;
        }
        s_chase_steps = words;                          // the whole cycle, every sample
        const uint32_t samples = words >= 16384u ? 8u : 32u;
        char label[REC_LABEL_LEN + 24];
        sample_access(chase, uncached, 0, samples, &s);
        snprintf(label, sizeof(label), "chase_all_uncached_%luk", (unsigned long)(sets[i] / 1024));
        rec_stats(ctx, label, UNIT_CYCLES, s_chase_steps, &s);
        snprintf(label, sizeof(label), "chase_all_uncached_%luk_mcyc", (unsigned long)(sets[i] / 1024));
        rec_value(ctx, label, UNIT_COUNT, (int64_t)((uint64_t)s.p50 * 1000u / s_chase_steps));
        guard_keepalive();

        sample_access(chase, cached, 0, samples, &s);
        snprintf(label, sizeof(label), "chase_all_cached_%luk_mcyc", (unsigned long)(sets[i] / 1024));
        rec_value(ctx, label, UNIT_COUNT, (int64_t)((uint64_t)s.p50 * 1000u / s_chase_steps));
        guard_keepalive();
    }
    rec_text(ctx, "chase_method", "each sample walks every word of the working set once in random order "
                                  "(since 2026-11-07; the old chase_* keys walked 256 steps only)");
    return TR_PASS;
}

// ---- Copies between SRAM and PSRAM -------------------------------------------------------------

static uint32_t s_copy_bytes;
static void *s_copy_dst;
static const void *s_copy_src;

static uint32_t __not_in_flash_func(do_memcpy)(void *unused, uint32_t n) {
    (void)unused; (void)n;
    memcpy(s_copy_dst, s_copy_src, s_copy_bytes);
    return *(volatile uint8_t *)s_copy_dst;
}

static int s_dma_ch = -1;

static uint32_t __not_in_flash_func(do_dma_copy)(void *unused, uint32_t n) {
    (void)unused; (void)n;
    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    dma_channel_configure(s_dma_ch, &c, s_copy_dst, s_copy_src, s_copy_bytes / 4u, true);
    dma_channel_wait_for_finish_blocking(s_dma_ch);
    return *(volatile uint8_t *)s_copy_dst;
}

static void record_copy(test_ctx_t *ctx, const char *label, bool dma, uint32_t clk_khz) {
    stats_t s;
    uint32_t samples = s_copy_bytes >= BIG_BYTES ? 16u : SAMPLES;
    sample_access(dma ? do_dma_copy : do_memcpy, NULL, 0, samples, &s);
    rec_stats(ctx, label, UNIT_CYCLES, s_copy_bytes, &s);
    char key[REC_LABEL_LEN + 24];
    snprintf(key, sizeof(key), "%.*s_rate", (int)(sizeof(key) - 6), label);
    rec_value(ctx, key, UNIT_KBPS, kbps(s_copy_bytes, s.p50, clk_khz));
    guard_keepalive();
}

test_result_t test_psram_copy(test_ctx_t *ctx) {
    if (!g_psram.present || !g_psram.initialised) return TR_NA;
    if (bench_ram_size() < BIG_BYTES * 2u) return TR_NA;
    s_dma_ch = dma_claim_unused_channel(false);
    if (s_dma_ch < 0) return TR_NA;

    uint8_t *sram = bench_ram();
    void *ps = (void *)PSRAM_NOCACHE_BASE;
    void *ps2 = (void *)(PSRAM_NOCACHE_BASE + 1024u * 1024u);
    static const uint32_t sizes[] = {4096, 65536};

    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        s_copy_bytes = sizes[i];
        unsigned kb = (unsigned)(sizes[i] / 1024);
        char label[REC_LABEL_LEN + 24];

        s_copy_src = sram; s_copy_dst = ps;
        snprintf(label, sizeof(label), "sram_to_psram_%uk", kb);
        record_copy(ctx, label, false, ctx->clk_khz);
        snprintf(label, sizeof(label), "sram_to_psram_%uk_dma", kb);
        record_copy(ctx, label, true, ctx->clk_khz);

        s_copy_src = ps; s_copy_dst = sram;
        snprintf(label, sizeof(label), "psram_to_sram_%uk", kb);
        record_copy(ctx, label, false, ctx->clk_khz);
        snprintf(label, sizeof(label), "psram_to_sram_%uk_dma", kb);
        record_copy(ctx, label, true, ctx->clk_khz);

        if (g_psram.size > 1024u * 1024u + sizes[i]) {
            s_copy_src = ps; s_copy_dst = ps2;
            snprintf(label, sizeof(label), "psram_to_psram_%uk", kb);
            record_copy(ctx, label, false, ctx->clk_khz);
            snprintf(label, sizeof(label), "psram_to_psram_%uk_dma", kb);
            record_copy(ctx, label, true, ctx->clk_khz);
        }
    }
    dma_channel_unclaim(s_dma_ch);
    s_dma_ch = -1;
    return TR_PASS;
}

// ---- QMI timing sweep -------------------------------------------------------------------------

// Write and read back an LFSR pattern; returns the number of bad words.
static uint32_t stress(uint32_t bytes) {
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_NOCACHE_BASE;
    uint32_t words = bytes / 4u;
    uint32_t seed = 0x2468ACE0u;
    uint32_t x = seed;
    for (uint32_t i = 0; i < words; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        p[i] = x;
    }
    x = seed;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < words; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        if (p[i] != x) bad++;
    }
    return bad;
}

// The SDK's own timing maths, so an explicit (divisor, rxdelay) keeps sane select times.
static int set_timing(uint32_t divisor, uint32_t rxdelay) {
    uint32_t clock_hz = clock_get_hz(clk_sys);
    uint32_t period_fs = (uint32_t)(1000000000000000ull / clock_hz);
    uint32_t max_select = (uint32_t)(((uint64_t)PICO_DEFAULT_PSRAM_MAX_SELECT * 1000000ull) /
                                     (64ull * (uint64_t)period_fs));
    // The hardware adds ceil(divisor / 2) cycles itself; at low clk_sys that alone covers
    // the chip's deselect time (the SDK's unsigned maths would wrap below zero there).
    uint32_t need = (PICO_DEFAULT_PSRAM_MIN_DESELECT * 1000000u + (period_fs - 1u)) / period_fs;
    uint32_t min_deselect = need > (divisor + 1u) / 2u ? need - (divisor + 1u) / 2u : 0u;
    int rc = psram_set_params(divisor, rxdelay, max_select, min_deselect);
    if (rc != PICO_OK) return rc;
    uint32_t irq = save_and_disable_interrupts();
    rc = psram_reinitialize();
    restore_interrupts(irq);
    xip_cache_invalidate_all();
    return rc;
}

// The fastest PSRAM SCK the sweep tries. 133 MHz is the APS6404's rating. The SDK keeps
// divisor >= 2 whenever clk_sys is above 100 MHz, so divisor 1 at 120 and 133 MHz is
// inside the chip's rating but outside the SDK's own rule - verified here like every
// other setting. The overclocking variant raises the limit.
#ifndef PICOBENCH_PSRAM_SCK_LIMIT_KHZ
#define PICOBENCH_PSRAM_SCK_LIMIT_KHZ 133000u
#endif

// Longest run of set bits in mask (bits 0..7): first bit and length.
static unsigned longest_run(uint32_t mask, unsigned *first) {
    unsigned best = 0, best_first = 0;
    for (unsigned i = 0; i < 8; i++) {
        unsigned n = 0;
        while (i + n < 8 && (mask & (1u << (i + n)))) n++;
        if (n > best) { best = n; best_first = i; }
    }
    *first = best_first;
    return best;
}

// Runs at every operating point (TF_SWEEP): the divisors whose SCK is within
// PICOBENCH_PSRAM_SCK_LIMIT_KHZ, fastest first, each with RX delays around the SDK's
// choice. The fastest stable divisor, at the centre of its RX delay window, is then
// stressed over the whole stress area again and measured: single uncached read latency
// and 4 KB uncached read rate.
test_result_t test_psram_timing_sweep(test_ctx_t *ctx) {
    if (!g_psram.present || !g_psram.initialised) return TR_NA;
    uint32_t clk_hz = clock_get_hz(clk_sys);
    uint32_t stress_bytes = g_psram.size < 262144u ? g_psram.size : 262144u;
    unsigned stable = 0;
    uint32_t div_min = (ctx->clk_khz + PICOBENCH_PSRAM_SCK_LIMIT_KHZ - 1u) / PICOBENCH_PSRAM_SCK_LIMIT_KHZ;
    if (div_min < 1u) div_min = 1u;
    uint32_t best_div = 0, best_rx = 0;

    for (uint32_t div = div_min; div <= 4; div++) {
        uint32_t mask = 0;
        for (uint32_t rx = div > 1u ? div - 1u : 0u; rx <= div + 2u && rx <= 7u; rx++) {
            guard_keepalive();
            char key[REC_LABEL_LEN + 24];
            snprintf(key, sizeof(key), "div%lu_rx%lu", (unsigned long)div, (unsigned long)rx);
            if (set_timing(div, rx) != PICO_OK) {
                rec_text(ctx, key, "parameters rejected by the SDK");
                continue;
            }
            uint32_t bad = stress(stress_bytes);
            stats_t s;
            sample_access(rd32, (void *)PSRAM_NOCACHE_BASE, BLOCK_BYTES, 32u, &s);
            uint32_t sck = clk_hz / 1000u / div;
            if (bad == 0) {
                stable++;
                mask |= 1u << rx;
                rec_text(ctx, key, "SCK %lu kHz, rxdelay %lu: stable, %lld kB/s", (unsigned long)sck,
                         (unsigned long)rx, (long long)kbps(BLOCK_BYTES, s.p50, ctx->clk_khz));
            } else {
                rec_text(ctx, key, "SCK %lu kHz, rxdelay %lu: %lu bad words out of %lu",
                         (unsigned long)sck, (unsigned long)rx, (unsigned long)bad,
                         (unsigned long)(stress_bytes / 4u));
            }
        }
        if (!best_div && mask) {
            unsigned first, len = longest_run(mask, &first);
            best_div = div;
            best_rx = first + (len - 1u) / 2u;
        }
    }

    if (best_div && set_timing(best_div, best_rx) == PICO_OK) {
        guard_keepalive();
        uint32_t bad = stress(stress_bytes) + stress(stress_bytes);
        rec_value(ctx, "best_div", UNIT_COUNT, best_div);
        rec_value(ctx, "best_rxdelay", UNIT_COUNT, best_rx);
        rec_value(ctx, "best_sck", UNIT_KHZ, clk_hz / 1000u / best_div);
        if (bad == 0) {
            stats_t s;
            sample_access(rd_one, (void *)PSRAM_NOCACHE_BASE, 0, SAMPLES, &s);
            rec_stats(ctx, "best_read_uncached", UNIT_CYCLES, 4, &s);
            record_access(ctx, "best_read32_4k", rd32, (void *)PSRAM_NOCACHE_BASE, BLOCK_BYTES, ctx->clk_khz);
        } else {
            rec_text(ctx, "best", "divisor %lu, rxdelay %lu: %lu bad words in the second check - not measured",
                     (unsigned long)best_div, (unsigned long)best_rx, (unsigned long)bad);
        }
    }

    // Always leave the chip on the SDK default profile, and prove it still works.
    if (!psram_retime(ctx)) return TR_FAIL;
    uint32_t bad = stress(stress_bytes);
    rec_value(ctx, "stable_combinations", UNIT_COUNT, stable);
    rec_value(ctx, "errors_after_restore", UNIT_COUNT, bad);
    rec_text(ctx, "note", "divisor and RX delay within the SDK's timing maths, SCK up to %lu kHz "
                          "(divisor 1 above 100 MHz clk_sys is outside the SDK's own rule); the "
                          "default profile is restored at the end",
             (unsigned long)PICOBENCH_PSRAM_SCK_LIMIT_KHZ);
    if (bad) {
        rec_error(ctx, "restore_failed", 0, 0, bad);
        return TR_FAIL;
    }
    return TR_PASS;
}

#endif
