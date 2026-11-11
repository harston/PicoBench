/*
 * SRAM benchmarks: throughput for 8/16/32-bit accesses, dependent-load latency for
 * random access, the effect of the access stride (bank conflicts), and how much a
 * concurrent DMA stream costs the CPU.
 *
 * Both chips stripe their large SRAM banks every 4 bytes, so a 4-byte stride rotates
 * through four banks while a 16-byte stride keeps hitting the same bank. That
 * difference is what the stride test measures, and it decides where a real-time
 * program should place its tables.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "hardware/dma.h"
#include "results/record.h"

#define BLOCK_BYTES 4096u
#define SAMPLES     256u

static volatile uint32_t s_sink;

// The largest free bank group: stride and latency tests need a big contiguous area.
static uint8_t *largest_group(uint32_t *size, unsigned *group) {
    uint8_t *best = NULL;
    *size = 0;
    for (unsigned g = 0; g < (unsigned)BENCH_BANK_GROUPS; g++) {
        uint32_t s;
        uint8_t *p = bench_group_ram(g, &s);
        if (s > *size) { *size = s; best = p; *group = g; }
    }
    return best;
}

static int64_t kbps(uint32_t bytes, uint32_t cycles, uint32_t clk_khz) {
    return cycles ? (int64_t)((uint64_t)bytes * clk_khz / cycles) : 0;
}

// ---- Access loops (all run from SRAM) -------------------------------------------------

static uint32_t __no_inline_not_in_flash_func(read32)(void *base, uint32_t bytes) {
    const uint32_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static uint32_t __not_in_flash_func(read32_reverse)(void *base, uint32_t bytes) {
    const uint32_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = bytes / 4u; i >= 8; i -= 8) {
        acc += p[i - 1] + p[i - 2] + p[i - 3] + p[i - 4] + p[i - 5] + p[i - 6] + p[i - 7] + p[i - 8];
    }
    return acc;
}

static uint32_t __not_in_flash_func(read16)(void *base, uint32_t bytes) {
    const uint16_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 2u; i += 8) {
        acc += (uint32_t)p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static uint32_t __not_in_flash_func(read8)(void *base, uint32_t bytes) {
    const uint8_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes; i += 8) {
        acc += (uint32_t)p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static uint32_t __not_in_flash_func(write32)(void *base, uint32_t bytes) {
    uint32_t *p = base;
    uint32_t v = 0x5A5A1234u;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
    return v;
}

static uint32_t __not_in_flash_func(write16)(void *base, uint32_t bytes) {
    uint16_t *p = base;
    uint16_t v = 0x1234;
    for (uint32_t i = 0; i < bytes / 2u; i += 8) {
        p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
    return v;
}

static uint32_t __not_in_flash_func(write8)(void *base, uint32_t bytes) {
    uint8_t *p = base;
    uint8_t v = 0x5A;
    for (uint32_t i = 0; i < bytes; i += 8) {
        p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
    return v;
}

#if !PICO_RP2040 && !defined(__riscv)
// A 32-bit load the compiler must not turn into LDM/LDRD: those need alignment and
// fault on an odd address, which is exactly what the misaligned test feeds them.
// Only Cortex-M33 allows unaligned word loads at all.
typedef struct { uint32_t v; } __attribute__((packed, aligned(1))) u32_unaligned_t;

static uint32_t __not_in_flash_func(read32_unaligned)(void *base, uint32_t bytes) {
    const uint8_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i + 32u <= bytes; i += 32u) {
        const uint8_t *q = p + i;
        acc += ((const u32_unaligned_t *)(q + 0))->v;
        acc += ((const u32_unaligned_t *)(q + 4))->v;
        acc += ((const u32_unaligned_t *)(q + 8))->v;
        acc += ((const u32_unaligned_t *)(q + 12))->v;
        acc += ((const u32_unaligned_t *)(q + 16))->v;
        acc += ((const u32_unaligned_t *)(q + 20))->v;
        acc += ((const u32_unaligned_t *)(q + 24))->v;
        acc += ((const u32_unaligned_t *)(q + 28))->v;
    }
    return acc;
}
#endif

typedef uint32_t (*access_fn)(void *base, uint32_t bytes);

/*
 * The access loops only read memory, so at -O3 the compiler happily inlines the
 * indirect call, proves the result loop-invariant and hoists the whole thing out of
 * the timed region - which reported 0 cycles. Calling through a volatile function
 * pointer, with the arguments passed through a register barrier, keeps the call where
 * it belongs.
 */
static void sample_access(access_fn fn, void *base, uint32_t bytes, stats_t *out) {
    access_fn volatile vfn = fn;
    s_sink += vfn(base, bytes);
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < SAMPLES; i++) {
        void *b = base;
        uint32_t n = bytes;
        __asm volatile("" : "+r"(b), "+r"(n));
        uint32_t t0 = timing_now();
        uint32_t r = vfn(b, n);
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    restore_interrupts(irq);
    stats_from_samples(g_bench_samples, SAMPLES, out);
}

static void record_access(test_ctx_t *ctx, const char *label, access_fn fn, void *base,
                          uint32_t bytes, uint32_t clk_khz) {
    stats_t s;
    sample_access(fn, base, bytes, &s);
    rec_stats(ctx, label, UNIT_CYCLES, bytes, &s);
    char key[REC_LABEL_LEN + 24];
    // Bounded precision: the label length is not known to the compiler.
    snprintf(key, sizeof(key), "%.*s_rate", (int)(sizeof(key) - 6), label);
    rec_value(ctx, key, UNIT_KBPS, kbps(bytes, s.p50, clk_khz));
    guard_keepalive();
}

test_result_t test_sram_throughput(test_ctx_t *ctx) {
    for (unsigned g = 0; g < (unsigned)BENCH_BANK_GROUPS; g++) {
        uint32_t size;
        uint8_t *ram = bench_group_ram(g, &size);
        if (size < BLOCK_BYTES) continue;
        char key[REC_LABEL_LEN], label[REC_LABEL_LEN + 24];
        snprintf(key, sizeof(key), "group%u", g);
        rec_text(ctx, key, "%s at 0x%08lx", bench_group_name(g), (unsigned long)(uintptr_t)ram);

        snprintf(label, sizeof(label), "g%u_read32_seq", g);
        record_access(ctx, label, read32, ram, BLOCK_BYTES, ctx->clk_khz);
        snprintf(label, sizeof(label), "g%u_read32_reverse", g);
        record_access(ctx, label, read32_reverse, ram, BLOCK_BYTES, ctx->clk_khz);
        snprintf(label, sizeof(label), "g%u_read16_seq", g);
        record_access(ctx, label, read16, ram, BLOCK_BYTES, ctx->clk_khz);
        snprintf(label, sizeof(label), "g%u_read8_seq", g);
        record_access(ctx, label, read8, ram, BLOCK_BYTES, ctx->clk_khz);
        snprintf(label, sizeof(label), "g%u_write32_seq", g);
        record_access(ctx, label, write32, ram, BLOCK_BYTES, ctx->clk_khz);
        snprintf(label, sizeof(label), "g%u_write16_seq", g);
        record_access(ctx, label, write16, ram, BLOCK_BYTES, ctx->clk_khz);
        snprintf(label, sizeof(label), "g%u_write8_seq", g);
        record_access(ctx, label, write8, ram, BLOCK_BYTES, ctx->clk_khz);

        // Unaligned 32-bit access: allowed on Cortex-M33, faults on Cortex-M0+ and is
        // not supported by Hazard3, so it is only measured where it is legal.
#if !PICO_RP2040 && !defined(__riscv)
        snprintf(label, sizeof(label), "g%u_read32_misaligned", g);
        record_access(ctx, label, read32_unaligned, ram + 1, BLOCK_BYTES - 32u, ctx->clk_khz);
#else
        snprintf(label, sizeof(label), "g%u_read32_misaligned", g);
        rec_text(ctx, label, "not supported by this core (unaligned 32-bit access faults)");
#endif
    }
    return TR_PASS;
}

// ---- Stride: bank conflicts -------------------------------------------------------------

static uint32_t s_stride_words;

// One read per stride step; `bytes` is reused as the stride in bytes. Unrolled with
// pointer increments: with an index multiply and a tight loop the overhead dominated
// and every stride measured the same.
static uint32_t __not_in_flash_func(read_stride)(void *base, uint32_t stride_bytes) {
    const uint8_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < s_stride_words; i += 8) {
        acc += *(const uint32_t *)p; p += stride_bytes;
        acc += *(const uint32_t *)p; p += stride_bytes;
        acc += *(const uint32_t *)p; p += stride_bytes;
        acc += *(const uint32_t *)p; p += stride_bytes;
        acc += *(const uint32_t *)p; p += stride_bytes;
        acc += *(const uint32_t *)p; p += stride_bytes;
        acc += *(const uint32_t *)p; p += stride_bytes;
        acc += *(const uint32_t *)p; p += stride_bytes;
    }
    return acc;
}

test_result_t test_sram_stride(test_ctx_t *ctx) {
    uint32_t size;
    unsigned g = 0;
    uint8_t *ram = largest_group(&size, &g);
    if (!ram || size < 64u * 1024u) return TR_NA;
    rec_text(ctx, "area", "%s, %lu KB free", bench_group_name(g), (unsigned long)(size / 1024));
    static const uint32_t strides[] = {4, 8, 16, 32, 64, 128, 256};
    s_stride_words = 1024;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (unsigned i = 0; i < sizeof(strides) / sizeof(strides[0]); i++) {
        if ((uint64_t)strides[i] * s_stride_words > size) break;
        stats_t s;
        sample_access(read_stride, ram, strides[i], &s);
        char label[REC_LABEL_LEN + 24];
        snprintf(label, sizeof(label), "read32_stride_%lu", (unsigned long)strides[i]);
        rec_stats(ctx, label, UNIT_CYCLES, s_stride_words, &s);
        snprintf(label, sizeof(label), "stride_%lu_mcyc_per_access", (unsigned long)strides[i]);
        rec_value(ctx, label, UNIT_COUNT, (int64_t)(s.p50 * 1000u / s_stride_words));
        if (s.p50 < lo) lo = s.p50;
        if (s.p50 > hi) hi = s.p50;
        guard_keepalive();
    }
    // Stride 4 rotates through the four striped banks; stride 16 and its multiples keep
    // hitting one bank. A single CPU issues loads one at a time, so a difference here
    // means the banks themselves are the limit.
    int64_t spread = lo ? (int64_t)(hi * 100u / lo) - 100 : 0;
    rec_value(ctx, "bank_conflict_effect_percent", UNIT_COUNT, spread);
    rec_text(ctx, "interpretation", "%s",
             spread <= 3 ? "no measurable bank-conflict effect for a single CPU master: the stride "
                           "does not change the cost of a load"
                         : "the stride changes the cost of a load: same-bank accesses are slower");
    return TR_PASS;
}

// ---- Random access latency (dependent loads) -----------------------------------------------

static uint32_t s_chase_steps;

// Pointer chase: each load depends on the previous one, so this is real access
// latency, not throughput - the number that matters for a cartridge lookup table.
static uint32_t __not_in_flash_func(chase)(void *base, uint32_t start) {
    const uint32_t *p = base;
    uint32_t idx = start;
    for (uint32_t i = 0; i < s_chase_steps; i += 8) {
        idx = p[idx]; idx = p[idx]; idx = p[idx]; idx = p[idx];
        idx = p[idx]; idx = p[idx]; idx = p[idx]; idx = p[idx];
    }
    return idx;
}

// Sattolo's algorithm: produces a permutation that is a single cycle of all `words`
// entries, in place and directly in "p[i] = next index" form, so the chase visits the
// whole buffer in random order.
static void build_chase(uint32_t *p, uint32_t words, uint32_t seed) {
    for (uint32_t i = 0; i < words; i++) p[i] = i;
    for (uint32_t i = words - 1; i > 0; i--) {
        uint32_t j = bench_rand(&seed) % i;          // strictly less than i: one cycle
        uint32_t t = p[i]; p[i] = p[j]; p[j] = t;
    }
}

test_result_t test_sram_latency(test_ctx_t *ctx) {
    static const uint32_t sizes[] = {4096, 16384, 65536, 262144};
    uint32_t avail;
    unsigned g = 0;
    uint8_t *ram = largest_group(&avail, &g);
    if (!ram || avail < 4096) return TR_NA;
    rec_text(ctx, "area", "%s, %lu KB free", bench_group_name(g), (unsigned long)(avail / 1024));
    s_chase_steps = 1024;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (sizes[i] > avail) break;
        guard_keepalive();
        build_chase((uint32_t *)ram, sizes[i] / 4u, 0xC0FFEEu + i);
        stats_t s;
        sample_access(chase, ram, 0, &s);
        char label[REC_LABEL_LEN + 24];
        snprintf(label, sizeof(label), "chase_%luk", (unsigned long)(sizes[i] / 1024));
        rec_stats(ctx, label, UNIT_CYCLES, s_chase_steps, &s);
        snprintf(label, sizeof(label), "chase_%luk_mcyc_per_load", (unsigned long)(sizes[i] / 1024));
        rec_value(ctx, label, UNIT_COUNT, (int64_t)(s.p50 * 1000u / s_chase_steps));
    }
    rec_text(ctx, "note", "dependent loads: each address comes from the previous load, so this is "
                          "access latency including the loop overhead");
    return TR_PASS;
}

// ---- CPU versus DMA -------------------------------------------------------------------------

/*
 * DMA traffic during a measurement.
 *
 * The DMA is restarted for each sample (outside the timed region) and aborted after
 * it, so a real SRAM->SRAM copy runs for the whole sample while the addresses stay
 * inside the buffers. A self-chaining pair of channels would restart with addresses
 * already advanced and walk out of the buffer; a single channel with an address ring
 * only produces traffic on one side and gave no measurable effect on RP2350, with no
 * way to tell "no contention" from "the DMA never ran". Now the transfers actually
 * completed during the samples are counted and reported, so the result is evidence.
 */
static int s_dma_ch = -1;
static uint32_t s_dma_words_done;

static bool dma_prepare(void) {
    s_dma_ch = dma_claim_unused_channel(false);
    return s_dma_ch >= 0;
}

static void dma_release(void) {
    if (s_dma_ch >= 0) {
        dma_channel_abort(s_dma_ch);
        dma_channel_unclaim(s_dma_ch);
        s_dma_ch = -1;
    }
}

// The DMA-to-CPU phase is swept: both the CPU read and the DMA copy walk the striped
// banks in order, so how often they collide depends on how far apart they start. With a
// fixed phase - set by the code between starting the DMA and the timed read, in flash -
// the RP2040's same-group slowdown read 24 % for several builds and then 36 %, with only
// the code layout changed. Each sample now starts the DMA, waits `delay` cycles and times
// the read, all from SRAM, and the delay steps over PHASE_SPAN cycles.
#define PHASE_SPAN 128u
#define DMA_PHASES 128u

static uint32_t __no_inline_not_in_flash_func(dma_sample)(access_fn fn, void *base, uint32_t bytes,
                                                          dma_channel_hw_t *ch, uint32_t src, uint32_t dst,
                                                          uint32_t words, uint32_t ctrl, uint32_t delay,
                                                          uint32_t *left) {
    uint32_t irq = save_and_disable_interrupts();
    ch->read_addr = src;
    ch->write_addr = dst;
    ch->transfer_count = words;
    ch->ctrl_trig = ctrl;                   // starts the copy
    uint32_t ts = timing_now();
    while (timing_elapsed(ts, timing_now()) < delay) {}
    __asm volatile("" : "+r"(base), "+r"(bytes));
    uint32_t t0 = timing_now();
    uint32_t r = fn(base, bytes);
    uint32_t t1 = timing_now();
    *left = ch->transfer_count & 0x0FFFFFFFu;
    restore_interrupts(irq);
    s_sink += r;
    return timing_net(t0, t1);
}

// Like sample_access(), but with an SRAM->SRAM DMA copy running across every sample, one
// sample per DMA-to-CPU phase.
static void sample_with_dma(access_fn fn, void *base, uint32_t bytes, void *dma_dst,
                            const void *dma_src, uint32_t dma_words, stats_t *out) {
    access_fn volatile vfn = fn;
    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    const uint32_t ctrl = channel_config_get_ctrl_value(&c);
    dma_channel_hw_t *ch = dma_channel_hw_addr(s_dma_ch);
    s_dma_words_done = 0;
    for (uint32_t i = 0; i < DMA_PHASES; i++) {
        uint32_t left = dma_words;
        g_bench_samples[i] = dma_sample(vfn, base, bytes, ch, (uint32_t)(uintptr_t)dma_src,
                                        (uint32_t)(uintptr_t)dma_dst, dma_words, ctrl,
                                        i * (PHASE_SPAN / DMA_PHASES), &left);
        dma_channel_abort(s_dma_ch);
        s_dma_words_done += dma_words - left;
    }
    stats_from_samples(g_bench_samples, DMA_PHASES, out);
}

static int64_t slowdown_pct(uint32_t busy, uint32_t quiet) {
    return quiet ? (int64_t)((uint64_t)busy * 100u / quiet) - 100 : 0;
}

// "Same group" is the bank group of the free heap (bench_ram()). On the RP2350 that is
// group 1 - the firmware's own data fills group 0 - and asking for group 0 returned 0
// bytes, so until 2026-11-07 this test reported N/A on every RP2350 (CLAUDE.md, SRAM
// notes). The other group's block comes from bench_group_block(), which falls back to the
// sample buffer beyond the words the samples themselves use.
test_result_t test_sram_contention_dma(test_ctx_t *ctx) {
    const unsigned home = bench_group_of(bench_ram());
    uint32_t size_home;
    uint8_t *gh = bench_group_ram(home, &size_home);
    if (size_home < 64u * 1024u) {
        rec_text(ctx, "skipped", "needs 64 KB free in bank group %u, %lu KB there", home,
                 (unsigned long)(size_home / 1024u));
        return TR_NA;
    }
    if (!dma_prepare()) {
        rec_text(ctx, "dma", "no free DMA channel");
        return TR_NA;
    }

    uint8_t *cpu_area = gh;
    const uint32_t dma_words = 4096;                 // 16 KB: outlasts one CPU sample
    uint8_t *dma_src_same = gh + 32u * 1024u;
    uint8_t *dma_dst_same = gh + 48u * 1024u;
    rec_value(ctx, "cpu_area_addr", UNIT_HEX, (int64_t)(uintptr_t)cpu_area);
    rec_value(ctx, "same_group", UNIT_COUNT, home);

    stats_t quiet, busy_same, busy_other;
    sample_access(read32, cpu_area, BLOCK_BYTES, &quiet);
    rec_stats(ctx, "cpu_read32_quiet", UNIT_CYCLES, BLOCK_BYTES, &quiet);
    rec_value(ctx, "cpu_read32_quiet_rate", UNIT_KBPS, kbps(BLOCK_BYTES, quiet.p50, ctx->clk_khz));
    guard_keepalive();

    // DMA copying inside the same bank group the CPU is reading. New keys (_swept): the
    // figures before 2026-11-07 came from one, layout-dependent phase.
    sample_with_dma(read32, cpu_area, BLOCK_BYTES, dma_dst_same, dma_src_same, dma_words, &busy_same);
    rec_stats(ctx, "cpu_read32_dma_same_swept", UNIT_CYCLES, BLOCK_BYTES, &busy_same);
    rec_value(ctx, "slowdown_same_p50_percent", UNIT_COUNT, slowdown_pct(busy_same.p50, quiet.p50));
    rec_value(ctx, "slowdown_same_min_percent", UNIT_COUNT, slowdown_pct(busy_same.min, quiet.p50));
    rec_value(ctx, "slowdown_same_max_percent", UNIT_COUNT, slowdown_pct(busy_same.max, quiet.p50));
    rec_value(ctx, "dma_words_same_group", UNIT_COUNT, s_dma_words_done);
    rec_value(ctx, "dma_rate_same_group", UNIT_KBPS,
              kbps(4u * s_dma_words_done / DMA_PHASES, busy_same.p50, ctx->clk_khz));
    if (s_dma_words_done == 0) rec_error(ctx, "dma_did_not_run", 0, dma_words, 0);
    guard_keepalive();

    // DMA in the other bank group (RP2350 only): the case that matters for real-time code.
    const char *source = "";
    uint8_t *go = BENCH_BANK_GROUPS > 1u ? bench_group_block(1u - home, 32u * 1024u, DMA_PHASES + 64u, &source)
                                         : NULL;
    if (go) {
        rec_value(ctx, "other_area_addr", UNIT_HEX, (int64_t)(uintptr_t)go);
        rec_text(ctx, "other_area", "bank group %u, %s", 1u - home, source);
        sample_with_dma(read32, cpu_area, BLOCK_BYTES, go + 16u * 1024u, go, dma_words, &busy_other);
        rec_stats(ctx, "cpu_read32_dma_other_swept", UNIT_CYCLES, BLOCK_BYTES, &busy_other);
        rec_value(ctx, "slowdown_other_p50_percent", UNIT_COUNT, slowdown_pct(busy_other.p50, quiet.p50));
        rec_value(ctx, "slowdown_other_max_percent", UNIT_COUNT, slowdown_pct(busy_other.max, quiet.p50));
        rec_value(ctx, "dma_words_other_group", UNIT_COUNT, s_dma_words_done);
        if (s_dma_words_done == 0) rec_error(ctx, "dma_did_not_run_other", 0, dma_words, 0);
    } else {
        rec_text(ctx, "other_group", BENCH_BANK_GROUPS > 1u ? "no 32 KB block free in the other bank group"
                                                            : "this chip has only one large bank group");
    }

    dma_release();
    rec_text(ctx, "note", "one DMA channel copies 16 KB of SRAM per sample, restarted outside the "
                          "timed region; dma_words_* proves how much it actually moved");
    rec_text(ctx, "phase", "DMA start to CPU read swept over 0-%u cycles, one sample per phase, all in "
                           "SRAM (since 2026-11-07); min/p50/max are over the phases",
             PHASE_SPAN - PHASE_SPAN / DMA_PHASES);
    return TR_PASS;
}
