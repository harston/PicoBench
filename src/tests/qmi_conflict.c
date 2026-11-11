/*
 * Flash and PSRAM share one QMI controller and one 16 KB XIP cache. This test measures
 * what each costs the other, which for a cartridge matters more than either one's peak
 * bandwidth: a PSRAM transfer running in the background must not stretch the worst-case
 * time to fetch a byte from flash.
 *
 * The DMA is restarted outside every timed sample and the words it actually moved are
 * counted, so "no effect" can be told apart from "the DMA never ran".
 *
 * Flash miss under PSRAM DMA (rewritten 2026-11): each sample - cache invalidate, DMA
 * start, delay, timed flash read - runs as one SRAM function, so no instruction is
 * fetched from flash after the invalidate. The delay between starting the DMA and the
 * flash read is swept over PHASE_SPAN cycles, which covers more than one PSRAM
 * max-select window (8 us = 1200 cycles at 150 MHz), and the result is the distribution
 * over those phases; its max is the worst case. The first version took one sample per
 * address at whatever phase the code layout produced: 514, 449 or 291 cycles depending
 * only on the build (see WORKLOG 2026-11-06).
 */

#include "tests/tests.h"

#include <stdio.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "detect/psram_info.h"
#include "results/record.h"

#if PICO_RP2040

test_result_t test_qmi_conflict(test_ctx_t *ctx) {
    (void)ctx;
    return TR_NA;   // RP2040 has no second QMI window
}

#else

#include "hardware/dma.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"

#define SAMPLES    128u
#define DMA_WORDS  4096u
#define BLOCK      4096u
#define PHASE_SPAN 2048u     // cycles swept between DMA start and the flash read
#define PHASE_STEP 8u
#define PHASES     (PHASE_SPAN / PHASE_STEP)

static volatile uint32_t s_sink;
static int s_dma_ch = -1;
static uint32_t s_dma_words_done;

static uint32_t __not_in_flash_func(rd32)(const void *base, uint32_t bytes) {
    const volatile uint32_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static int64_t pct(uint32_t busy, uint32_t quiet) {
    return quiet ? (int64_t)((uint64_t)busy * 100u / quiet) - 100 : 0;
}

// One flash-miss sample, entirely in SRAM: invalidate, optionally start the DMA, wait
// `delay` cycles, then time one read from a fresh flash line. Returns the cycles; the
// DMA keeps running and is aborted by the caller (outside the timed part).
static uint32_t __no_inline_not_in_flash_func(miss_sample)(const volatile uint32_t *addr,
                                                           dma_channel_hw_t *ch, uint32_t src,
                                                           uint32_t dst, uint32_t ctrl,
                                                           uint32_t delay, uint32_t *words_left) {
    xip_cache_invalidate_all();         // SDK: also __no_inline_not_in_flash_func
    uint32_t irq = save_and_disable_interrupts();
    if (ch) {
        ch->read_addr = src;
        ch->write_addr = dst;
        ch->transfer_count = DMA_WORDS;
        ch->ctrl_trig = ctrl;           // starts the transfer
    }
    uint32_t ts = timing_now();
    while (timing_elapsed(ts, timing_now()) < delay) {}
    __asm volatile("" : "+r"(addr));
    uint32_t t0 = timing_now();
    uint32_t v = *addr;
    uint32_t t1 = timing_now();
    if (ch) *words_left = ch->transfer_count & 0x0FFFFFFFu;
    restore_interrupts(irq);
    s_sink += v;
    return timing_net(t0, t1);
}

static const volatile uint32_t *miss_addr(uint32_t i) {
    return (const volatile uint32_t *)(XIP_BASE + (uintptr_t)(i % 256u) * 64u);
}

static void sample_flash_miss_quiet(stats_t *out) {
    for (uint32_t i = 0; i < SAMPLES; i++)
        g_bench_samples[i] = miss_sample(miss_addr(i), NULL, 0, 0, 0, 0, NULL);
    stats_from_samples(g_bench_samples, SAMPLES, out);
}

typedef struct {
    const char *name;       // label stem
    uint32_t src, dst;
    bool read_inc, write_inc;
} dma_pattern_t;

// One sample per phase: the DMA started `phase * PHASE_STEP` cycles before the read.
// Records the distribution over phases, the worst phase and the words the DMA moved.
static uint32_t sweep_flash_miss(test_ctx_t *ctx, const dma_pattern_t *pat, uint32_t quiet_p50) {
    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, pat->read_inc);
    channel_config_set_write_increment(&c, pat->write_inc);
    uint32_t ctrl = channel_config_get_ctrl_value(&c);
    dma_channel_hw_t *ch = dma_channel_hw_addr(s_dma_ch);

    uint32_t worst = 0, worst_phase = 0, moved = 0;
    for (uint32_t k = 0; k < PHASES; k++) {
        uint32_t left = DMA_WORDS;
        uint32_t cyc = miss_sample(miss_addr(k), ch, pat->src, pat->dst, ctrl, k * PHASE_STEP, &left);
        dma_channel_abort(s_dma_ch);
        moved += DMA_WORDS - left;
        g_bench_samples[k] = cyc;
        if (cyc > worst) { worst = cyc; worst_phase = k * PHASE_STEP; }
    }
    stats_t s;
    stats_from_samples(g_bench_samples, PHASES, &s);

    char key[REC_LABEL_LEN];
    snprintf(key, sizeof key, "miss_dma_%s", pat->name);
    rec_stats(ctx, key, UNIT_CYCLES, 1, &s);
    snprintf(key, sizeof key, "miss_dma_%s_worst_at", pat->name);
    rec_value(ctx, key, UNIT_CYCLES, worst_phase);
    snprintf(key, sizeof key, "miss_dma_%s_words", pat->name);
    rec_value(ctx, key, UNIT_COUNT, moved);
    snprintf(key, sizeof key, "miss_dma_%s_worst_pct", pat->name);
    rec_value(ctx, key, UNIT_COUNT, pct(worst, quiet_p50));
    if (moved == 0) rec_error(ctx, "dma_did_not_run", 0, DMA_WORDS, 0);
    return worst;
}

// Streaming read from `base`, optionally with DMA traffic on the other device.
static void sample_stream(const void *base, bool with_dma, void *dma_dst, const void *dma_src,
                          stats_t *out) {
    s_dma_words_done = 0;
    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    uint32_t samples = 32;
    for (uint32_t i = 0; i < samples; i++) {
        if (with_dma) dma_channel_configure(s_dma_ch, &c, dma_dst, dma_src, DMA_WORDS, true);
        const void *b = base;
        __asm volatile("" : "+r"(b));
        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = timing_now();
        uint32_t r = rd32(b, BLOCK);
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        if (with_dma) {
            s_dma_words_done += DMA_WORDS - dma_channel_hw_addr(s_dma_ch)->transfer_count;
            dma_channel_abort(s_dma_ch);
        }
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    stats_from_samples(g_bench_samples, samples, out);
}

test_result_t test_qmi_conflict(test_ctx_t *ctx) {
    if (!g_psram.present || !g_psram.initialised) {
        rec_text(ctx, "psram", "not present: nothing shares the QMI with flash");
        return TR_NA;
    }
    if (bench_ram_size() < 64u * 1024u) return TR_NA;
    s_dma_ch = dma_claim_unused_channel(false);
    if (s_dma_ch < 0) return TR_NA;

    void *ps = (void *)PSRAM_NOCACHE_BASE;
    void *ps2 = (void *)(PSRAM_NOCACHE_BASE + 512u * 1024u);
    uint8_t *sram = bench_ram();
    stats_t quiet, busy;

    // 1. Worst case for a cartridge: a flash cache miss while PSRAM is busy.
    xip_cache_clean_all();      // write back cached PSRAM data before invalidating
    sample_flash_miss_quiet(&quiet);
    rec_stats(ctx, "flash_miss_quiet", UNIT_CYCLES, 1, &quiet);
    guard_keepalive();
    const dma_pattern_t patterns[] = {
        {"psram_read", (uint32_t)(uintptr_t)ps, (uint32_t)(uintptr_t)sram, true, true},
        {"psram_write", (uint32_t)(uintptr_t)sram, (uint32_t)(uintptr_t)ps2, true, true},
        {"psram_copy", (uint32_t)(uintptr_t)ps, (uint32_t)(uintptr_t)ps2, true, true},
    };
    uint32_t worst = 0;
    for (unsigned i = 0; i < sizeof patterns / sizeof patterns[0]; i++) {
        uint32_t w = sweep_flash_miss(ctx, &patterns[i], quiet.p50);
        if (w > worst) worst = w;
        guard_keepalive();
    }
    rec_value(ctx, "flash_miss_worst_with_psram", UNIT_CYCLES, worst);
    rec_value(ctx, "flash_miss_worst_pct", UNIT_COUNT, pct(worst, quiet.p50));
    rec_text(ctx, "miss_method", "each sample in SRAM: invalidate, start the DMA, wait 0-%u cycles "
                                 "(step %u), time one flash read; distribution over the %u phases. "
                                 "Patterns: psram_read = PSRAM->SRAM, psram_write = SRAM->PSRAM, "
                                 "psram_copy = PSRAM->PSRAM. Not comparable with figures before 2026-11.",
             PHASE_SPAN - PHASE_STEP, PHASE_STEP, PHASES);

    // 2. Flash streaming read with PSRAM DMA running.
    sample_stream((const void *)XIP_BASE, false, NULL, NULL, &quiet);
    rec_stats(ctx, "flash_stream_quiet", UNIT_CYCLES, BLOCK, &quiet);
    sample_stream((const void *)XIP_BASE, true, ps2, ps, &busy);
    rec_stats(ctx, "flash_stream_psram_dma", UNIT_CYCLES, BLOCK, &busy);
    rec_value(ctx, "flash_stream_slowdown_percent", UNIT_COUNT, pct(busy.p50, quiet.p50));
    guard_keepalive();

    // 3. The other direction: PSRAM reads while DMA streams from flash into SRAM. The DMA
    //    reads flash through the uncached alias, so every word reaches the chip. Through
    //    the cached alias its traffic depended on which lines of the image happened to be
    //    cached: 2 % slowdown in results_8, 20 % in results_9, with only the code layout
    //    changed. New keys (_unc), so older figures are not compared with these.
    sample_stream(ps, false, NULL, NULL, &quiet);
    rec_stats(ctx, "psram_stream_quiet", UNIT_CYCLES, BLOCK, &quiet);
    sample_stream(ps, true, sram, (const void *)XIP_NOCACHE_NOALLOC_BASE, &busy);
    rec_stats(ctx, "psram_stream_flash_dma_unc", UNIT_CYCLES, BLOCK, &busy);
    rec_value(ctx, "psram_stream_slowdown_pct_unc", UNIT_COUNT, pct(busy.p50, quiet.p50));
    rec_value(ctx, "dma_words_flash", UNIT_COUNT, s_dma_words_done);

    dma_channel_abort(s_dma_ch);
    dma_channel_unclaim(s_dma_ch);
    s_dma_ch = -1;
    rec_text(ctx, "note", "both devices hang off the same QMI and share the 16 KB XIP cache; the "
                          "DMA moves 16 KB per sample and the words moved are reported as proof");
    rec_text(ctx, "flash_dma_source", "uncached alias, every word from the chip (since 2026-11-07; "
                                      "the cached alias made the figure depend on code layout)");
    return TR_PASS;
}

#endif
