/*
 * DMA: every channel, transfer sizes, chaining, pacing and channels running in parallel.
 * The SRAM-versus-CPU contention case is measured in sram.contention_dma (phase 4);
 * here the subject is the DMA engine itself.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "results/record.h"

#define SAMPLES 64u

static int64_t kbps(uint32_t bytes, uint32_t cycles, uint32_t clk_khz) {
    return cycles ? (int64_t)((uint64_t)bytes * clk_khz / cycles) : 0;
}

// One transfer, timed from configure to completion.
static uint32_t timed_copy(int ch, void *dst, const void *src, uint32_t words, uint dreq) {
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    if (dreq != DREQ_FORCE) channel_config_set_dreq(&c, dreq);
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = timing_now();
    dma_channel_configure(ch, &c, dst, src, words, true);
    dma_channel_wait_for_finish_blocking(ch);
    uint32_t t1 = timing_now();
    restore_interrupts(irq);
    return timing_net(t0, t1);
}

test_result_t test_dma_channels(test_ctx_t *ctx) {
    if (bench_ram_size() < 32u * 1024u) return TR_NA;
    uint8_t *src = bench_ram();
    uint8_t *dst = bench_ram() + 16u * 1024u;
    for (uint32_t i = 0; i < 4096; i++) ((uint32_t *)src)[i] = i * 2654435761u;

    rec_value(ctx, "dma_channels", UNIT_COUNT, NUM_DMA_CHANNELS);
    uint32_t failures = 0;
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        guard_keepalive();
        char key[REC_LABEL_LEN];
        snprintf(key, sizeof(key), "channel%u", ch);
        if (dma_channel_is_claimed(ch)) {
            rec_text(ctx, key, "in use by the firmware, skipped");
            continue;
        }
        dma_channel_claim(ch);
        memset(dst, 0, 1024);
        uint32_t cycles = timed_copy((int)ch, dst, src, 256, DREQ_FORCE);
        bool ok = memcmp(dst, src, 1024) == 0;
        if (!ok) {
            rec_error(ctx, key, ch, 0, 1);
            failures++;
        }
        rec_text(ctx, key, "%s, 1 KB in %lu cycles", ok ? "ok" : "DATA MISMATCH", (unsigned long)cycles);
        dma_channel_unclaim(ch);
    }
    rec_value(ctx, "failures", UNIT_COUNT, failures);
    return failures ? TR_FAIL : TR_PASS;
}

test_result_t test_dma_transfer(test_ctx_t *ctx) {
    if (bench_ram_size() < 192u * 1024u) {
        rec_text(ctx, "note", "not enough free SRAM for the large sizes; smaller ones only");
    }
    int ch = dma_claim_unused_channel(false);
    if (ch < 0) return TR_NA;
    uint8_t *src = bench_ram();
    uint32_t half = bench_ram_size() / 2u;
    uint8_t *dst = bench_ram() + (half & ~4095u);

    static const uint32_t sizes[] = {64, 256, 1024, 4096, 16384, 65536};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint32_t bytes = sizes[i];
        if (bytes * 2u > bench_ram_size()) break;
        for (uint32_t k = 0; k < SAMPLES; k++) {
            g_bench_samples[k] = timed_copy(ch, dst, src, bytes / 4u, DREQ_FORCE);
        }
        stats_t s;
        stats_from_samples(g_bench_samples, SAMPLES, &s);
        char label[REC_LABEL_LEN + 24];
        snprintf(label, sizeof(label), "copy_%lu", (unsigned long)bytes);
        rec_stats(ctx, label, UNIT_CYCLES, bytes, &s);
        snprintf(label, sizeof(label), "copy_%lu_rate", (unsigned long)bytes);
        rec_value(ctx, label, UNIT_KBPS, kbps(bytes, s.p50, ctx->clk_khz));
        guard_keepalive();
    }
    // Setup overhead: the smallest possible transfer shows what a transfer costs before
    // any data moves, which is what matters when DMA is used for short bursts.
    for (uint32_t k = 0; k < SAMPLES; k++) g_bench_samples[k] = timed_copy(ch, dst, src, 1, DREQ_FORCE);
    stats_t s;
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "single_word_transfer", UNIT_CYCLES, 1, &s);
    dma_channel_unclaim(ch);
    return TR_PASS;
}

test_result_t test_dma_parallel(test_ctx_t *ctx) {
    if (bench_ram_size() < 128u * 1024u) return TR_NA;
    const uint32_t words = 4096;          // 16 KB per channel
    uint8_t *base = bench_ram();
    static const unsigned counts[] = {1, 2, 4, 8};

    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        unsigned n = counts[c];
        if (n * 2u * words * 4u > bench_ram_size()) break;
        int ch[8];
        unsigned claimed = 0;
        for (unsigned i = 0; i < n; i++) {
            ch[i] = dma_claim_unused_channel(false);
            if (ch[i] < 0) break;
            claimed++;
        }
        if (claimed == n) {
            uint32_t mask = 0;
            for (unsigned i = 0; i < n; i++) {
                dma_channel_config cfg = dma_channel_get_default_config(ch[i]);
                channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
                channel_config_set_read_increment(&cfg, true);
                channel_config_set_write_increment(&cfg, true);
                dma_channel_configure(ch[i], &cfg, base + (2u * i + 1u) * words * 4u,
                                      base + 2u * i * words * 4u, words, false);
                mask |= 1u << ch[i];
            }
            uint32_t irq = save_and_disable_interrupts();
            uint32_t t0 = timing_now();
            dma_start_channel_mask(mask);      // all at once
            while (dma_hw->ch[ch[0]].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) {}
            for (unsigned i = 1; i < n; i++) {
                while (dma_hw->ch[ch[i]].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) {}
            }
            uint32_t t1 = timing_now();
            restore_interrupts(irq);
            uint32_t cycles = timing_net(t0, t1);
            char key[REC_LABEL_LEN];
            snprintf(key, sizeof(key), "channels_%u_rate", n);
            rec_value(ctx, key, UNIT_KBPS, kbps(n * words * 4u, cycles, ctx->clk_khz));
            snprintf(key, sizeof(key), "channels_%u_cycles", n);
            rec_value(ctx, key, UNIT_CYCLES, cycles);
        }
        for (unsigned i = 0; i < claimed; i++) dma_channel_unclaim(ch[i]);
        guard_keepalive();
    }
    rec_text(ctx, "note", "identical SRAM-to-SRAM copies started together; the aggregate rate shows "
                          "where the bus, not the channel, becomes the limit");
    return TR_PASS;
}

test_result_t test_dma_chain_and_pacing(test_ctx_t *ctx) {
    if (bench_ram_size() < 64u * 1024u) return TR_NA;
    int a = dma_claim_unused_channel(false);
    int b = dma_claim_unused_channel(false);
    if (a < 0 || b < 0) return TR_NA;
    uint8_t *src = bench_ram();
    uint8_t *dst = bench_ram() + 32u * 1024u;
    const uint32_t words = 1024;

    // Chaining: A finishes and starts B without the CPU.
    dma_channel_config cb = dma_channel_get_default_config(b);
    channel_config_set_transfer_data_size(&cb, DMA_SIZE_32);
    channel_config_set_read_increment(&cb, true);
    channel_config_set_write_increment(&cb, true);
    dma_channel_configure(b, &cb, dst + words * 4u, src + words * 4u, words, false);

    dma_channel_config ca = dma_channel_get_default_config(a);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
    channel_config_set_read_increment(&ca, true);
    channel_config_set_write_increment(&ca, true);
    channel_config_set_chain_to(&ca, b);

    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = timing_now();
    dma_channel_configure(a, &ca, dst, src, words, true);
    dma_channel_wait_for_finish_blocking(a);
    dma_channel_wait_for_finish_blocking(b);
    uint32_t t1 = timing_now();
    restore_interrupts(irq);
    uint32_t chained = timing_net(t0, t1);
    rec_value(ctx, "chained_two_transfers", UNIT_CYCLES, chained);
    rec_value(ctx, "chained_rate", UNIT_KBPS, kbps(2u * words * 4u, chained, ctx->clk_khz));
    guard_keepalive();

    // Pacing: a DMA timer throttles the transfer to a fraction of clk_sys.
    int timer = dma_claim_unused_timer(false);
    if (timer >= 0) {
        // x/y of clk_sys: 1/64 here, so one word per 64 system clocks.
        dma_timer_set_fraction((uint)timer, 1, 64);
        uint32_t cycles = 0;
        dma_channel_config cp = dma_channel_get_default_config(a);
        channel_config_set_transfer_data_size(&cp, DMA_SIZE_32);
        channel_config_set_read_increment(&cp, true);
        channel_config_set_write_increment(&cp, true);
        channel_config_set_dreq(&cp, dma_get_timer_dreq((uint)timer));
        irq = save_and_disable_interrupts();
        t0 = timing_now();
        dma_channel_configure(a, &cp, dst, src, 256, true);
        dma_channel_wait_for_finish_blocking(a);
        t1 = timing_now();
        restore_interrupts(irq);
        cycles = timing_net(t0, t1);
        rec_value(ctx, "paced_256_words_cycles", UNIT_CYCLES, cycles);
        rec_value(ctx, "paced_mcyc_per_word", UNIT_COUNT, (int64_t)((uint64_t)cycles * 1000u / 256u));
        rec_text(ctx, "pacing", "DMA timer set to 1/64 of clk_sys: expected 64 cycles per word");
        dma_timer_unclaim((uint)timer);
    } else {
        rec_text(ctx, "pacing", "no free DMA timer");
    }

    dma_channel_unclaim(a);
    dma_channel_unclaim(b);
    return TR_PASS;
}
