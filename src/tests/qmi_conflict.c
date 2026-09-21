/*
 * Flash and PSRAM share one QMI controller and one 16 KB XIP cache. This test measures
 * what each costs the other, which for a cartridge matters more than either one's peak
 * bandwidth: a PSRAM transfer running in the background must not stretch the worst-case
 * time to fetch a byte from flash.
 *
 * The DMA is restarted outside every timed sample and the words it actually moved are
 * counted, so "no effect" can be told apart from "the DMA never ran".
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

static uint32_t __not_in_flash_func(rd_one)(const void *base) {
    return *(const volatile uint32_t *)base;
}

// Cold flash miss: invalidate, then one read from a fresh address.
static void sample_flash_miss(bool with_dma, void *dma_dst, const void *dma_src, stats_t *out) {
    s_dma_words_done = 0;
    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    for (uint32_t i = 0; i < SAMPLES; i++) {
        xip_cache_invalidate_all();
        if (with_dma) dma_channel_configure(s_dma_ch, &c, dma_dst, dma_src, DMA_WORDS, true);
        const void *addr = (const void *)(XIP_BASE + (uintptr_t)i * 64u);
        __asm volatile("" : "+r"(addr));
        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = timing_now();
        uint32_t r = rd_one(addr);
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        if (with_dma) {
            s_dma_words_done += DMA_WORDS - dma_channel_hw_addr(s_dma_ch)->transfer_count;
            dma_channel_abort(s_dma_ch);
        }
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    stats_from_samples(g_bench_samples, SAMPLES, out);
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

static int64_t pct(uint32_t busy, uint32_t quiet) {
    return quiet ? (int64_t)((uint64_t)busy * 100u / quiet) - 100 : 0;
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
    sample_flash_miss(false, NULL, NULL, &quiet);
    rec_stats(ctx, "flash_miss_quiet", UNIT_CYCLES, 1, &quiet);
    guard_keepalive();
    sample_flash_miss(true, ps2, ps, &busy);
    rec_stats(ctx, "flash_miss_psram_dma", UNIT_CYCLES, 1, &busy);
    rec_value(ctx, "flash_miss_slowdown_percent", UNIT_COUNT, pct(busy.p50, quiet.p50));
    rec_value(ctx, "flash_miss_worst_quiet", UNIT_CYCLES, quiet.max);
    rec_value(ctx, "flash_miss_worst_with_psram", UNIT_CYCLES, busy.max);
    rec_value(ctx, "dma_words_psram", UNIT_COUNT, s_dma_words_done);
    if (s_dma_words_done == 0) rec_error(ctx, "dma_did_not_run", 0, DMA_WORDS, 0);
    guard_keepalive();

    // 2. Flash streaming read with PSRAM DMA running.
    sample_stream((const void *)XIP_BASE, false, NULL, NULL, &quiet);
    rec_stats(ctx, "flash_stream_quiet", UNIT_CYCLES, BLOCK, &quiet);
    sample_stream((const void *)XIP_BASE, true, ps2, ps, &busy);
    rec_stats(ctx, "flash_stream_psram_dma", UNIT_CYCLES, BLOCK, &busy);
    rec_value(ctx, "flash_stream_slowdown_percent", UNIT_COUNT, pct(busy.p50, quiet.p50));
    guard_keepalive();

    // 3. The other direction: PSRAM reads while DMA streams from flash into SRAM.
    sample_stream(ps, false, NULL, NULL, &quiet);
    rec_stats(ctx, "psram_stream_quiet", UNIT_CYCLES, BLOCK, &quiet);
    sample_stream(ps, true, sram, (const void *)XIP_BASE, &busy);
    rec_stats(ctx, "psram_stream_flash_dma", UNIT_CYCLES, BLOCK, &busy);
    rec_value(ctx, "psram_stream_slowdown_percent", UNIT_COUNT, pct(busy.p50, quiet.p50));
    rec_value(ctx, "dma_words_flash", UNIT_COUNT, s_dma_words_done);

    dma_channel_abort(s_dma_ch);
    dma_channel_unclaim(s_dma_ch);
    s_dma_ch = -1;
    rec_text(ctx, "note", "both devices hang off the same QMI and share the 16 KB XIP cache; the "
                          "DMA moves 16 KB per sample and the words moved are reported as proof");
    return TR_PASS;
}

#endif
