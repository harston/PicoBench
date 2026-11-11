/*
 * Flash / XIP / cache.
 *
 * The XIP cache is 16 KB with 8-byte lines on both chips and is shared by flash and
 * PSRAM. What matters for a real-time program is not the streaming bandwidth but the
 * cost of a single cache miss, so every latency here is reported as a distribution and
 * the worst case is part of the result.
 *
 * The read kernels run from SRAM, so only the data access goes to flash. The cold-miss
 * sample runs from SRAM as a whole (bench_cold_reads): until 2026-11 only the read was in
 * SRAM and the loop around it in flash, so the instruction fetches after each invalidate
 * added ~110 (dual) / ~60 (quad) cycles to read_miss_cold.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "hardware/address_mapped.h"
#include "hardware/regs/addressmap.h"
#include "hardware/xip_cache.h"
#include "results/record.h"
#include "storage/flash_layout.h"

#define SAMPLES     256u
#define BLOCK_BYTES 4096u
#define BIG_BYTES   65536u

// Read from the firmware image itself: always programmed, never erased, identical on
// every board of the same build.
#define FLASH_READ_OFFSET 0u

static volatile uint32_t s_sink;

static const uint8_t *cached_ptr(uint32_t off) { return (const uint8_t *)(XIP_BASE + off); }
static const uint8_t *uncached_ptr(uint32_t off) { return (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + off); }

static int64_t kbps(uint32_t bytes, uint32_t cycles, uint32_t clk_khz) {
    return cycles ? (int64_t)((uint64_t)bytes * clk_khz / cycles) : 0;
}

// ---- Read kernels (in SRAM) ---------------------------------------------------------

static uint32_t __no_inline_not_in_flash_func(rd32)(void *base, uint32_t bytes) {
    const uint32_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static uint32_t __no_inline_not_in_flash_func(rd16)(void *base, uint32_t bytes) {
    const uint16_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 2u; i += 8) {
        acc += (uint32_t)p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static uint32_t __no_inline_not_in_flash_func(rd8)(void *base, uint32_t bytes) {
    const uint8_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes; i += 8) {
        acc += (uint32_t)p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

// One read per cache line: every access is a miss when the region is larger than the cache.
static uint32_t __no_inline_not_in_flash_func(rd_lines)(void *base, uint32_t bytes) {
    const uint8_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes; i += 8u * XIP_CACHE_LINE_SIZE) {
        acc += *(const uint32_t *)(p + i);
        acc += *(const uint32_t *)(p + i + 1u * XIP_CACHE_LINE_SIZE);
        acc += *(const uint32_t *)(p + i + 2u * XIP_CACHE_LINE_SIZE);
        acc += *(const uint32_t *)(p + i + 3u * XIP_CACHE_LINE_SIZE);
        acc += *(const uint32_t *)(p + i + 4u * XIP_CACHE_LINE_SIZE);
        acc += *(const uint32_t *)(p + i + 5u * XIP_CACHE_LINE_SIZE);
        acc += *(const uint32_t *)(p + i + 6u * XIP_CACHE_LINE_SIZE);
        acc += *(const uint32_t *)(p + i + 7u * XIP_CACHE_LINE_SIZE);
    }
    return acc;
}

// A single 32-bit read: the unit of a cache miss or hit.
static uint32_t __no_inline_not_in_flash_func(rd_one)(void *base, uint32_t unused) {
    (void)unused;
    return *(const volatile uint32_t *)base;
}

typedef uint32_t (*read_fn)(void *base, uint32_t bytes);

// Flash is slow: keep every measurement at roughly 1 MB of traffic instead of a fixed
// sample count, so a 64 KB block does not read 16 MB through a 30 MB/s interface.
static uint32_t samples_for(uint32_t bytes) {
    uint32_t n = bytes ? (1u << 20) / bytes : SAMPLES;
    if (n < 16u) n = 16u;
    if (n > SAMPLES) n = SAMPLES;
    return n;
}

// The sampling loop runs from SRAM. Run from flash, its own instruction fetches shared
// the XIP port and cache with the data being measured: with the same arrangement the
// PSRAM cache hit read 8 cycles instead of 4 (psram_bench.c, results_12).
static void __no_inline_not_in_flash_func(sample_loop)(read_fn fn, const void *base, uint32_t bytes,
                                                       uint32_t samples) {
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < samples; i++) {
        void *b = (void *)base;
        uint32_t n = bytes;
        __asm volatile("" : "+r"(b), "+r"(n));
        uint32_t t0 = timing_now();
        uint32_t r = fn(b, n);
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    restore_interrupts(irq);
}

static void sample_read(read_fn fn, const void *base, uint32_t bytes, stats_t *out) {
    read_fn volatile vfn = fn;
    uint32_t samples = samples_for(bytes);
    s_sink += vfn((void *)base, bytes);
    sample_loop(vfn, base, bytes, samples);
    stats_from_samples(g_bench_samples, samples, out);
}

// Each sample reads a fresh address with a cold cache: the cost of one miss.
static void sample_cold_read(const uint8_t *base, uint32_t stride, stats_t *out) {
    bench_cold_reads(base, stride, SAMPLES, g_bench_samples);
    stats_from_samples(g_bench_samples, SAMPLES, out);
}

static void record_read(test_ctx_t *ctx, const char *label, read_fn fn, const void *base,
                        uint32_t bytes, uint32_t clk_khz) {
    stats_t s;
    sample_read(fn, base, bytes, &s);
    rec_stats(ctx, label, UNIT_CYCLES, bytes, &s);
    char key[REC_LABEL_LEN + 24];
    snprintf(key, sizeof(key), "%.*s_rate", (int)(sizeof(key) - 6), label);
    rec_value(ctx, key, UNIT_KBPS, kbps(bytes, s.p50, clk_khz));
    guard_keepalive();
}

// ---- Throughput -----------------------------------------------------------------------

test_result_t test_flash_read(test_ctx_t *ctx) {
    const uint8_t *c = cached_ptr(FLASH_READ_OFFSET);
    const uint8_t *u = uncached_ptr(FLASH_READ_OFFSET);

    rec_text(ctx, "cache", "%lu KB, %lu-byte lines, shared with PSRAM",
             (unsigned long)(XIP_CACHE_SIZE / 1024), (unsigned long)XIP_CACHE_LINE_SIZE);
    rec_text(ctx, "sampling", "sampling loop in SRAM (since 2026-11-07); before, its instruction fetches "
                              "from flash shared the XIP port with the reads");

    // 4 KB fits in the 16 KB cache: after the warm-up every access is a hit.
    record_read(ctx, "cached_warm_read32_4k", rd32, c, BLOCK_BYTES, ctx->clk_khz);
    record_read(ctx, "cached_warm_read16_4k", rd16, c, BLOCK_BYTES, ctx->clk_khz);
    record_read(ctx, "cached_warm_read8_4k", rd8, c, BLOCK_BYTES, ctx->clk_khz);

    // 64 KB is four times the cache, so this streams from the flash chip.
    record_read(ctx, "cached_stream_read32_64k", rd32, c, BIG_BYTES, ctx->clk_khz);

    // The uncached alias never allocates: this is the raw interface bandwidth.
    record_read(ctx, "uncached_read32_4k", rd32, u, BLOCK_BYTES, ctx->clk_khz);
    record_read(ctx, "uncached_read32_64k", rd32, u, BIG_BYTES, ctx->clk_khz);
    record_read(ctx, "uncached_read8_4k", rd8, u, BLOCK_BYTES, ctx->clk_khz);
    return TR_PASS;
}

// ---- Cache behaviour ---------------------------------------------------------------------

test_result_t test_flash_cache(test_ctx_t *ctx) {
    const uint8_t *c = cached_ptr(FLASH_READ_OFFSET);
    const uint8_t *u = uncached_ptr(FLASH_READ_OFFSET);
    stats_t s;

    rec_text(ctx, "sampling", "sampling loop in SRAM (since 2026-11-07): read_hit and the working-set "
                              "figures are not comparable with earlier builds");
    // Hit: the same line, already cached.
    s_sink += rd_one((void *)c, 0);
    sample_read(rd_one, c, 0, &s);
    rec_stats(ctx, "read_hit", UNIT_CYCLES, 1, &s);
    rec_value(ctx, "read_hit_mcyc", UNIT_COUNT, (int64_t)(s.p50 * 1000u));

    // Miss: cache invalidated before each sample, and a fresh address every time.
    sample_cold_read(c, 64, &s);
    rec_stats(ctx, "read_miss_cold", UNIT_CYCLES, 1, &s);
    rec_value(ctx, "read_miss_cold_mcyc", UNIT_COUNT, (int64_t)(s.p50 * 1000u));
    rec_value(ctx, "read_miss_worst_cycles", UNIT_CYCLES, s.max);
    rec_text(ctx, "miss_method", "whole sample in SRAM (invalidate, read); figures before "
                                 "2026-11 included instruction fetches from flash");
    guard_keepalive();

    // Uncached alias: always goes to the chip, no allocation.
    sample_read(rd_one, u, 0, &s);
    rec_stats(ctx, "read_uncached", UNIT_CYCLES, 1, &s);

    // One read per cache line over a region four times the cache: every access misses.
    sample_read(rd_lines, c, BIG_BYTES, &s);
    uint32_t lines = BIG_BYTES / XIP_CACHE_LINE_SIZE;
    rec_stats(ctx, "read_per_line_64k", UNIT_CYCLES, lines, &s);
    rec_value(ctx, "per_line_miss_mcyc", UNIT_COUNT, (int64_t)(s.p50 * 1000u / lines));
    guard_keepalive();

    // Working set sweep: where the 16 KB cache stops holding the data.
    static const uint32_t sets[] = {2048, 4096, 8192, 16384, 32768, 65536};
    for (unsigned i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        sample_read(rd32, c, sets[i], &s);
        char label[REC_LABEL_LEN + 24];
        snprintf(label, sizeof(label), "working_set_%luk_mcyc_per_word", (unsigned long)(sets[i] / 1024));
        rec_value(ctx, label, UNIT_COUNT, (int64_t)(s.p50 * 1000u / (sets[i] / 4u)));
        guard_keepalive();
    }

    // Cost of the maintenance operation itself.
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = timing_now();
    xip_cache_invalidate_all();
    uint32_t t1 = timing_now();
    restore_interrupts(irq);
    rec_value(ctx, "invalidate_all", UNIT_CYCLES, timing_net(t0, t1));
    return TR_PASS;
}

// ---- Code execution from flash versus SRAM ------------------------------------------------

// Deliberately left in flash: this is the code under test.
static uint32_t work_in_flash(uint32_t n) {
    uint32_t a = n | 1u;
    for (uint32_t i = 0; i < 256; i++) {
        a = a * 5u + 1u;
        __asm volatile("" : "+r"(a));
    }
    return a;
}

static uint32_t __not_in_flash_func(work_in_sram)(uint32_t n) {
    uint32_t a = n | 1u;
    for (uint32_t i = 0; i < 256; i++) {
        a = a * 5u + 1u;
        __asm volatile("" : "+r"(a));
    }
    return a;
}

typedef uint32_t (*work_fn)(uint32_t);

static void sample_work(work_fn fn, bool cold, uint32_t samples, stats_t *out) {
    work_fn volatile vfn = fn;
    if (samples > SAMPLES) samples = SAMPLES;
    s_sink += vfn(1);
    for (uint32_t i = 0; i < samples; i++) {
        if (cold) xip_cache_invalidate_all();
        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = timing_now();
        uint32_t r = vfn(i);
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    stats_from_samples(g_bench_samples, samples, out);
}

test_result_t test_flash_code_exec(test_ctx_t *ctx) {
    stats_t sram, flash_warm, flash_cold;
    sample_work(work_in_sram, false, SAMPLES, &sram);
    rec_stats(ctx, "loop_from_sram", UNIT_CYCLES, 256, &sram);
    guard_keepalive();

    sample_work(work_in_flash, false, SAMPLES, &flash_warm);
    rec_stats(ctx, "loop_from_flash_warm", UNIT_CYCLES, 256, &flash_warm);
    guard_keepalive();

    sample_work(work_in_flash, true, SAMPLES, &flash_cold);
    rec_stats(ctx, "loop_from_flash_cold", UNIT_CYCLES, 256, &flash_cold);

    rec_value(ctx, "flash_warm_overhead_percent", UNIT_COUNT,
              sram.p50 ? (int64_t)(flash_warm.p50 * 100u / sram.p50) - 100 : 0);
    rec_value(ctx, "flash_cold_overhead_percent", UNIT_COUNT,
              sram.p50 ? (int64_t)(flash_cold.p50 * 100u / sram.p50) - 100 : 0);
    rec_value(ctx, "flash_cold_worst_cycles", UNIT_CYCLES, flash_cold.max);
    rec_text(ctx, "note", "identical loop, only the location differs; 'cold' invalidates the whole "
                          "cache before each call, so the code is fetched from flash again");
    return TR_PASS;
}

// ---- Flash to SRAM copy ----------------------------------------------------------------------

static uint32_t s_copy_bytes;
static uint8_t *s_copy_dst;
static const uint8_t *s_copy_src;

static uint32_t __not_in_flash_func(do_copy)(uint32_t n) {
    (void)n;
    memcpy(s_copy_dst, s_copy_src, s_copy_bytes);
    return s_copy_dst[0];
}

test_result_t test_flash_copy_to_sram(test_ctx_t *ctx) {
    if (bench_ram_size() < BIG_BYTES + 1024u) return TR_NA;
    s_copy_dst = bench_ram();
    static const uint32_t sizes[] = {1024, 4096, 16384, 65536};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        s_copy_bytes = sizes[i];
        char label[REC_LABEL_LEN + 24];
        stats_t s;

        s_copy_src = cached_ptr(FLASH_READ_OFFSET);
        sample_work(do_copy, false, samples_for(s_copy_bytes), &s);
        snprintf(label, sizeof(label), "copy_cached_%luk", (unsigned long)(sizes[i] / 1024));
        rec_stats(ctx, label, UNIT_CYCLES, s_copy_bytes, &s);
        snprintf(label, sizeof(label), "copy_cached_%luk_rate", (unsigned long)(sizes[i] / 1024));
        rec_value(ctx, label, UNIT_KBPS, kbps(s_copy_bytes, s.p50, ctx->clk_khz));
        guard_keepalive();

        s_copy_src = uncached_ptr(FLASH_READ_OFFSET);
        sample_work(do_copy, false, samples_for(s_copy_bytes), &s);
        snprintf(label, sizeof(label), "copy_uncached_%luk_rate", (unsigned long)(sizes[i] / 1024));
        rec_value(ctx, label, UNIT_KBPS, kbps(s_copy_bytes, s.p50, ctx->clk_khz));
        guard_keepalive();
    }
    rec_text(ctx, "note", "memcpy from XIP into SRAM; the cached variant reuses the 16 KB cache, "
                          "the uncached one always reads the chip");
    return TR_PASS;
}
