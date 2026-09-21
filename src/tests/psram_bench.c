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

static uint32_t __not_in_flash_func(rd32)(void *base, uint32_t bytes) {
    const volatile uint32_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static uint32_t __not_in_flash_func(rd8)(void *base, uint32_t bytes) {
    const volatile uint8_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes; i += 8) {
        acc += (uint32_t)p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static uint32_t __not_in_flash_func(wr32)(void *base, uint32_t bytes) {
    volatile uint32_t *p = base;
    uint32_t v = 0x5A5A1234u;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
    return v;
}

static uint32_t __not_in_flash_func(wr8)(void *base, uint32_t bytes) {
    volatile uint8_t *p = base;
    uint8_t v = 0x5A;
    for (uint32_t i = 0; i < bytes; i += 8) {
        p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
    return v;
}

// A single read: the unit of latency.
static uint32_t __not_in_flash_func(rd_one)(void *base, uint32_t unused) {
    (void)unused;
    return *(const volatile uint32_t *)base;
}

// Dependent reads: each address comes from the previous load.
static uint32_t s_chase_steps;
static uint32_t __not_in_flash_func(chase)(void *base, uint32_t start) {
    const volatile uint32_t *p = base;
    uint32_t idx = start;
    for (uint32_t i = 0; i < s_chase_steps; i += 8) {
        idx = p[idx]; idx = p[idx]; idx = p[idx]; idx = p[idx];
        idx = p[idx]; idx = p[idx]; idx = p[idx]; idx = p[idx];
    }
    return idx;
}

typedef uint32_t (*access_fn)(void *base, uint32_t arg);

static void sample_access(access_fn fn, void *base, uint32_t arg, uint32_t samples, stats_t *out) {
    access_fn volatile vfn = fn;   // opaque: keeps the call inside the timed region
    if (samples > BENCH_SAMPLES_MAX) samples = BENCH_SAMPLES_MAX;
    s_sink += vfn(base, arg);
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < samples; i++) {
        void *b = base;
        uint32_t a = arg;
        __asm volatile("" : "+r"(b), "+r"(a));
        uint32_t t0 = timing_now();
        uint32_t r = vfn(b, a);
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    restore_interrupts(irq);
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
    for (uint32_t i = 0; i < SAMPLES; i++) {
        xip_cache_invalidate_all();
        uint32_t irq = save_and_disable_interrupts();
        void *b = (void *)((uintptr_t)cached + (size_t)i * 64u);
        __asm volatile("" : "+r"(b));
        uint32_t t0 = timing_now();
        uint32_t r = rd_one(b, 0);
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "read_miss_cold", UNIT_CYCLES, 1, &s);
    rec_value(ctx, "read_miss_worst_cycles", UNIT_CYCLES, s.max);
    guard_keepalive();

    sample_access(rd_one, uncached, 0, SAMPLES, &s);
    rec_stats(ctx, "read_uncached_single", UNIT_CYCLES, 1, &s);

    // Dependent-load latency over working sets that do and do not fit the cache.
    static const uint32_t sets[] = {16384, 262144};
    s_chase_steps = 256;
    for (unsigned i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        if (sets[i] > g_psram.size) break;
        volatile uint32_t *p = uncached;
        uint32_t words = sets[i] / 4u;
        uint32_t seed = 0xBEEF0001u + i;
        for (uint32_t k = 0; k < words; k++) p[k] = k;
        for (uint32_t k = words - 1; k > 0; k--) {     // Sattolo: one cycle over all entries
            uint32_t j = bench_rand(&seed) % k;
            uint32_t t = p[k]; p[k] = p[j]; p[j] = t;
        }
        guard_keepalive();
        char label[REC_LABEL_LEN + 24];
        sample_access(chase, uncached, 0, SAMPLES, &s);
        snprintf(label, sizeof(label), "chase_uncached_%luk", (unsigned long)(sets[i] / 1024));
        rec_stats(ctx, label, UNIT_CYCLES, s_chase_steps, &s);
        snprintf(label, sizeof(label), "chase_uncached_%luk_mcyc", (unsigned long)(sets[i] / 1024));
        rec_value(ctx, label, UNIT_COUNT, (int64_t)(s.p50 * 1000u / s_chase_steps));

        sample_access(chase, cached, 0, SAMPLES, &s);
        snprintf(label, sizeof(label), "chase_cached_%luk_mcyc", (unsigned long)(sets[i] / 1024));
        rec_value(ctx, label, UNIT_COUNT, (int64_t)(s.p50 * 1000u / s_chase_steps));
        guard_keepalive();
    }
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
    uint32_t min_deselect = (PICO_DEFAULT_PSRAM_MIN_DESELECT * 1000000u + (period_fs - 1u)) / period_fs -
                            (divisor + 1u) / 2u;
    int rc = psram_set_params(divisor, rxdelay, max_select, min_deselect);
    if (rc != PICO_OK) return rc;
    uint32_t irq = save_and_disable_interrupts();
    rc = psram_reinitialize();
    restore_interrupts(irq);
    xip_cache_invalidate_all();
    return rc;
}

test_result_t test_psram_timing_sweep(test_ctx_t *ctx) {
    if (!g_psram.present || !g_psram.initialised) return TR_NA;
    uint32_t clk_hz = clock_get_hz(clk_sys);
    uint32_t stress_bytes = g_psram.size < 262144u ? g_psram.size : 262144u;
    unsigned stable = 0;

    for (uint32_t div = 4; div >= 2; div--) {
        for (uint32_t rx = div > 0 ? div - 1u : 0u; rx <= div + 1u && rx <= 7u; rx++) {
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
                rec_text(ctx, key, "SCK %lu kHz, rxdelay %lu: stable, %lld kB/s", (unsigned long)sck,
                         (unsigned long)rx, (long long)kbps(BLOCK_BYTES, s.p50, ctx->clk_khz));
            } else {
                rec_text(ctx, key, "SCK %lu kHz, rxdelay %lu: %lu bad words out of %lu",
                         (unsigned long)sck, (unsigned long)rx, (unsigned long)bad,
                         (unsigned long)(stress_bytes / 4u));
            }
        }
    }

    // Always leave the chip on the SDK default profile, and prove it still works.
    if (!psram_retime(ctx)) return TR_FAIL;
    uint32_t bad = stress(stress_bytes);
    rec_value(ctx, "stable_combinations", UNIT_COUNT, stable);
    rec_value(ctx, "errors_after_restore", UNIT_COUNT, bad);
    rec_text(ctx, "note", "divisor and RX delay only, within the SDK's own timing maths; the "
                          "default profile (max 133 MHz) is restored at the end");
    if (bad) {
        rec_error(ctx, "restore_failed", 0, 0, bad);
        return TR_FAIL;
    }
    return TR_PASS;
}

#endif
