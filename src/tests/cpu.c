/*
 * CPU benchmarks. Everything here runs from SRAM with interrupts disabled, so the
 * numbers are the core's own timing without XIP or interrupt effects.
 *
 * Each measurement takes many samples of a fixed amount of work and reports
 * min/p50/p95/p99/p99.9/max in cycles. For per-operation costs an empty loop of the
 * same shape is measured first and subtracted; the result is scaled by the number of
 * operations per iteration and reported in milli-cycles per operation.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "results/crc32.h"
#include "results/record.h"

#define SAMPLES 512u
#define ITERS   200u      // loop iterations per sample
#define UNROLL  8u        // operations per iteration

// Sinks the compiler cannot optimise away.
static volatile uint32_t s_sink32;
static volatile uint8_t s_sink8;

// ---- Sampling helper ---------------------------------------------------------------

// Runs fn(arg) `SAMPLES` times, filling g_bench_samples with net cycles per call.
// Called through a volatile pointer so the optimiser cannot inline the kernel into
// the timed region and hoist it out of the loop (that reported 0 cycles).
static void sample(uint32_t (*fn)(uint32_t), uint32_t arg, stats_t *out) {
    uint32_t (*volatile vfn)(uint32_t) = fn;
    s_sink32 += vfn(arg);   // warm up
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < SAMPLES; i++) {
        uint32_t a = arg;
        __asm volatile("" : "+r"(a));
        uint32_t t0 = timing_now();
        uint32_t r = vfn(a);
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink32 += r;
    }
    restore_interrupts(irq);
    stats_from_samples(g_bench_samples, SAMPLES, out);
}

// Records a per-operation cost: (measured - empty loop) / (ITERS * UNROLL), in
// milli-cycles so fractions of a cycle are visible.
static void record_op(test_ctx_t *ctx, const char *label, const stats_t *s, uint32_t base_p50) {
    rec_stats(ctx, label, UNIT_CYCLES, ITERS * UNROLL, s);
    uint64_t net = s->p50 > base_p50 ? s->p50 - base_p50 : 0;
    char key[REC_LABEL_LEN + 24];
    snprintf(key, sizeof(key), "%s_mcycles_per_op", label);
    rec_value(ctx, key, UNIT_COUNT, (int64_t)(net * 1000u / (ITERS * UNROLL)));
}

// ---- Integer operations -------------------------------------------------------------

/*
 * A register barrier after every operation. Without it the compiler rewrites the
 * repeated expression algebraically - eight multiplies by a constant become one, eight
 * adds become "a + 8b" - and the measurement reported a fraction of a cycle per
 * "operation". The barrier emits no instruction; it only forbids merging.
 */
#define FENCE(v) __asm volatile("" : "+r"(v))

// Same loop shape as OP_LOOP but with no operation: the baseline that is subtracted.
static uint32_t __not_in_flash_func(loop_empty)(uint32_t n) {
    uint32_t a = n | 1u;
    for (uint32_t i = 0; i < ITERS; i++) {
        FENCE(a); FENCE(a); FENCE(a); FENCE(a);
        FENCE(a); FENCE(a); FENCE(a); FENCE(a);
    }
    return a;
}

#define OP_LOOP(name, expr)                                        \
    static uint32_t __not_in_flash_func(name)(uint32_t n) {        \
        uint32_t a = n | 1u, b = 0x9E3779B9u;                      \
        FENCE(b); /* opaque: a constant divisor would become mul+shift */ \
        (void)b;                                                   \
        for (uint32_t i = 0; i < ITERS; i++) {                     \
            expr; FENCE(a); expr; FENCE(a);                        \
            expr; FENCE(a); expr; FENCE(a);                        \
            expr; FENCE(a); expr; FENCE(a);                        \
            expr; FENCE(a); expr; FENCE(a);                        \
        }                                                          \
        return a;                                                  \
    }

OP_LOOP(loop_add, a = a + b)
OP_LOOP(loop_mul, a = a * b)
OP_LOOP(loop_shift, a = (a << 3) | (a >> 29))
OP_LOOP(loop_udiv, a = a / (b | 1u) + 1u)
OP_LOOP(loop_sdiv, a = (uint32_t)((int32_t)a / (int32_t)(b | 1u)) + 1u)
OP_LOOP(loop_and_or, a = (a & b) | (a ^ b))

test_result_t test_cpu_integer(test_ctx_t *ctx) {
    stats_t base, s;
    sample(loop_empty, 1, &base);
    rec_stats(ctx, "empty_loop", UNIT_CYCLES, ITERS, &base);
    rec_value(ctx, "empty_loop_mcycles_per_iter", UNIT_COUNT, (int64_t)(base.p50 * 1000u / ITERS));

    guard_keepalive();
    sample(loop_add, 1, &s);   record_op(ctx, "add32", &s, base.p50);
    sample(loop_and_or, 1, &s); record_op(ctx, "and_or32", &s, base.p50);
    sample(loop_shift, 1, &s); record_op(ctx, "shift32", &s, base.p50);
    guard_keepalive();
    sample(loop_mul, 1, &s);   record_op(ctx, "mul32", &s, base.p50);
    sample(loop_udiv, 1, &s);  record_op(ctx, "udiv32", &s, base.p50);
    sample(loop_sdiv, 1, &s);  record_op(ctx, "sdiv32", &s, base.p50);
#if PICO_RP2040
    rec_text(ctx, "divider", "RP2040 has no divide instruction: the SIO hardware divider is used");
#elif defined(__riscv)
    rec_text(ctx, "divider", "Hazard3 M extension (div/divu)");
#else
    rec_text(ctx, "divider", "Cortex-M33 UDIV/SDIV");
#endif
    return TR_PASS;
}

// ---- Branches -------------------------------------------------------------------------

// Always-taken branch: fully predictable.
static uint32_t __not_in_flash_func(loop_branch_pred)(uint32_t n) {
    uint32_t a = n;
    for (uint32_t i = 0; i < ITERS * UNROLL; i++) {
        if (i & 1u) a += 3; else a ^= 7;
    }
    return a;
}

// Data-dependent branch driven by pseudo-random bits.
static uint32_t __not_in_flash_func(loop_branch_rand)(uint32_t n) {
    uint32_t a = n, r = 0x12345678u;
    for (uint32_t i = 0; i < ITERS * UNROLL; i++) {
        r = r * 1103515245u + 12345u;
        if (r & 0x10000u) a += 3; else a ^= 7;
    }
    return a;
}

test_result_t test_cpu_branch(test_ctx_t *ctx) {
    stats_t pred, rnd;
    sample(loop_branch_pred, 1, &pred);
    sample(loop_branch_rand, 1, &rnd);
    rec_stats(ctx, "branch_predictable", UNIT_CYCLES, ITERS * UNROLL, &pred);
    rec_stats(ctx, "branch_data_dependent", UNIT_CYCLES, ITERS * UNROLL, &rnd);
    rec_value(ctx, "predictable_mcycles_per_branch", UNIT_COUNT,
              (int64_t)(pred.p50 * 1000u / (ITERS * UNROLL)));
    // The random variant also contains the PRNG, so the difference is an upper bound
    // on the cost of an unpredictable branch, not the cost itself.
    rec_value(ctx, "random_minus_predictable", UNIT_CYCLES, (int64_t)rnd.p50 - (int64_t)pred.p50);
    return TR_PASS;
}

// ---- memcpy / memset in SRAM -----------------------------------------------------------

static uint32_t s_bytes;
static uint8_t *s_src, *s_dst;

static uint32_t __not_in_flash_func(do_memcpy)(uint32_t n) {
    (void)n;
    memcpy(s_dst, s_src, s_bytes);
    return s_dst[0];
}

static uint32_t __not_in_flash_func(do_memset)(uint32_t n) {
    memset(s_dst, (int)n, s_bytes);
    return s_dst[0];
}

// 32-bit word copy loop, for comparison with the library memcpy.
static uint32_t __not_in_flash_func(do_word_copy)(uint32_t n) {
    (void)n;
    const uint32_t *s = (const uint32_t *)s_src;
    uint32_t *d = (uint32_t *)s_dst;
    for (uint32_t i = 0; i < s_bytes / 4u; i += 4) {
        d[i] = s[i];
        d[i + 1] = s[i + 1];
        d[i + 2] = s[i + 2];
        d[i + 3] = s[i + 3];
    }
    return d[0];
}

// Throughput in kB/s from cycles for `bytes` at this operating point.
static int64_t kbps(uint32_t bytes, uint32_t cycles, uint32_t clk_khz) {
    if (!cycles) return 0;
    return (int64_t)((uint64_t)bytes * clk_khz / cycles);
}

test_result_t test_cpu_memory_ops(test_ctx_t *ctx) {
    uint32_t ram_size = bench_ram_size();
    if (ram_size < 64u * 1024u) {
        rec_text(ctx, "skipped", "not enough free SRAM (%lu bytes)", (unsigned long)ram_size);
        return TR_NA;
    }
    s_src = bench_ram();
    s_dst = bench_ram() + 32u * 1024u;
    memset(s_src, 0xA5, 16u * 1024u);

    static const uint32_t sizes[] = {64, 256, 1024, 4096, 16384};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        guard_keepalive();
        s_bytes = sizes[i];
        stats_t s;
        char label[REC_LABEL_LEN + 24];

        sample(do_memcpy, 0, &s);
        snprintf(label, sizeof(label), "memcpy_%lu", (unsigned long)s_bytes);
        rec_stats(ctx, label, UNIT_CYCLES, s_bytes, &s);
        snprintf(label, sizeof(label), "memcpy_%lu_rate", (unsigned long)s_bytes);
        rec_value(ctx, label, UNIT_KBPS, kbps(s_bytes, s.p50, ctx->clk_khz));

        sample(do_word_copy, 0, &s);
        snprintf(label, sizeof(label), "word_copy_%lu", (unsigned long)s_bytes);
        rec_stats(ctx, label, UNIT_CYCLES, s_bytes, &s);

        sample(do_memset, 0x5A, &s);
        snprintf(label, sizeof(label), "memset_%lu", (unsigned long)s_bytes);
        rec_stats(ctx, label, UNIT_CYCLES, s_bytes, &s);
        snprintf(label, sizeof(label), "memset_%lu_rate", (unsigned long)s_bytes);
        rec_value(ctx, label, UNIT_KBPS, kbps(s_bytes, s.p50, ctx->clk_khz));
    }
    return TR_PASS;
}

// ---- Software CRC32 ---------------------------------------------------------------------

static uint32_t __not_in_flash_func(do_crc32)(uint32_t n) {
    (void)n;
    return crc32_update(0, s_src, s_bytes);
}

test_result_t test_cpu_crc32(test_ctx_t *ctx) {
    if (bench_ram_size() < 8u * 1024u) return TR_NA;
    s_src = bench_ram();
    s_bytes = 4096;
    memset(s_src, 0x3C, s_bytes);
    stats_t s;
    sample(do_crc32, 0, &s);
    rec_stats(ctx, "crc32_sw_4k", UNIT_CYCLES, s_bytes, &s);
    rec_value(ctx, "crc32_mcycles_per_byte", UNIT_COUNT, (int64_t)(s.p50 * 1000u / s_bytes));
    rec_value(ctx, "crc32_rate", UNIT_KBPS, kbps(s_bytes, s.p50, ctx->clk_khz));
    rec_text(ctx, "implementation", "table-less nibble CRC-32 (results/crc32.c), no hardware CRC on either chip");
    return TR_PASS;
}

// ---- Lookup table: the shape of the cartridge hot path ------------------------------------

static const uint8_t *s_lut;
static uint32_t s_lut_mask;

// Random index into a LUT, as a cartridge address decode would do.
static uint32_t __not_in_flash_func(do_lut_random)(uint32_t n) {
    uint32_t r = n, acc = 0;
    for (uint32_t i = 0; i < ITERS; i++) {
        r = bench_rand(&r);
        acc += s_lut[r & s_lut_mask];
    }
    s_sink8 = (uint8_t)acc;
    return acc;
}

// Sequential walk of the same table, for comparison.
static uint32_t __not_in_flash_func(do_lut_seq)(uint32_t n) {
    uint32_t acc = 0;
    for (uint32_t i = 0; i < ITERS; i++) {
        acc += s_lut[(n + i) & s_lut_mask];
    }
    s_sink8 = (uint8_t)acc;
    return acc;
}

test_result_t test_cpu_lut(test_ctx_t *ctx) {
    static const uint32_t sizes[] = {4096, 16384, 65536};
    uint32_t ram = bench_ram_size();
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (sizes[i] > ram) break;
        guard_keepalive();
        s_lut = bench_ram();
        s_lut_mask = sizes[i] - 1u;
        memset(bench_ram(), 0x42, sizes[i]);
        stats_t s;
        char label[REC_LABEL_LEN + 24];

        sample(do_lut_random, 0x2545F491u, &s);
        snprintf(label, sizeof(label), "lut_random_%luk", (unsigned long)(sizes[i] / 1024));
        rec_stats(ctx, label, UNIT_CYCLES, ITERS, &s);
        snprintf(label, sizeof(label), "lut_random_%luk_mcyc", (unsigned long)(sizes[i] / 1024));
        rec_value(ctx, label, UNIT_COUNT, (int64_t)(s.p50 * 1000u / ITERS));

        sample(do_lut_seq, 7, &s);
        snprintf(label, sizeof(label), "lut_seq_%luk", (unsigned long)(sizes[i] / 1024));
        rec_stats(ctx, label, UNIT_CYCLES, ITERS, &s);
    }
    rec_text(ctx, "note", "SRAM lookups including the PRNG that produces the index; the "
                          "cartridge hot path itself is measured in its own test (phase 7)");
    return TR_PASS;
}
