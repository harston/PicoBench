/*
 * Interrupts and timers: what an interrupt costs and, more importantly, how late it can
 * be. Phase 7 showed that a single 10 kHz interrupt is the largest disturbance of the
 * cartridge hot path, so the distribution here matters more than the median.
 *
 * Measured:
 *   - alarm latency: the gap between the requested time and the first instruction of
 *     the handler, in cycles,
 *   - handler entry and exit cost for a plain software interrupt,
 *   - the same under load (DMA running),
 *   - an interrupt storm: the worst foreground delay while alarms fire continuously.
 */

#include "tests/tests.h"

#include <stdio.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include "results/record.h"

#define SAMPLES 256u
#define SPIN_LIMIT 1000000u   // a handler that never runs must not hang the stage

static volatile uint32_t s_isr_cycles;
static volatile uint32_t s_isr_count;
static volatile uint32_t s_sink;
static int s_alarm = -1;
static uint32_t s_target_cycles;

// Runs in interrupt context: record when the handler actually started.
static void alarm_handler(uint alarm_num) {
    (void)alarm_num;
    s_isr_cycles = timing_now();
    s_isr_count++;
}

// A spare interrupt with nothing else attached, used to time entry and exit.
#define SPARE_IRQ ((uint)(NUM_IRQS - 1u))

static void spare_handler(void) {
    s_isr_cycles = timing_now();
    s_isr_count++;
}

test_result_t test_irq_latency(test_ctx_t *ctx) {
    // ---- Software interrupt: entry and round trip ----
    irq_set_exclusive_handler(SPARE_IRQ, spare_handler);
    irq_set_enabled(SPARE_IRQ, true);
    for (uint32_t i = 0; i < SAMPLES; i++) {
        s_isr_count = 0;
        uint32_t t0 = timing_now();
        irq_set_pending(SPARE_IRQ);
        uint32_t spins = 0;
        while (!s_isr_count && ++spins < SPIN_LIMIT) {}
        uint32_t t1 = timing_now();
        if (!s_isr_count) {
            rec_error(ctx, "sw_irq_never_fired", SPARE_IRQ, 1, 0);
            irq_set_enabled(SPARE_IRQ, false);
            irq_remove_handler(SPARE_IRQ, spare_handler);
            return TR_FAIL;
        }
        g_bench_samples[i] = timing_net(t0, s_isr_cycles);   // entry only
        s_sink += timing_net(t0, t1);
    }
    stats_t s;
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "sw_irq_entry", UNIT_CYCLES, 1, &s);

    for (uint32_t i = 0; i < SAMPLES; i++) {
        s_isr_count = 0;
        uint32_t t0 = timing_now();
        irq_set_pending(SPARE_IRQ);
        uint32_t spins = 0;
        while (!s_isr_count && ++spins < SPIN_LIMIT) {}
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_net(t0, t1);             // entry + handler + return
    }
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "sw_irq_round_trip", UNIT_CYCLES, 1, &s);
    irq_set_enabled(SPARE_IRQ, false);
    irq_remove_handler(SPARE_IRQ, spare_handler);
    guard_keepalive();

    // ---- Hardware alarm: how late is "on time"? ----
    s_alarm = hardware_alarm_claim_unused(false);
    if (s_alarm < 0) {
        rec_text(ctx, "alarm", "no free hardware alarm");
        return TR_PASS;
    }
    hardware_alarm_set_callback((uint)s_alarm, alarm_handler);
    const uint32_t delay_us = 200;
    uint32_t cycles_per_us = ctx->clk_khz / 1000u;
    for (uint32_t i = 0; i < SAMPLES; i++) {
        s_isr_count = 0;
        uint32_t t0 = timing_now();
        s_target_cycles = delay_us * cycles_per_us;
        if (hardware_alarm_set_target((uint)s_alarm, make_timeout_time_us(delay_us))) {
            rec_error(ctx, "alarm_target_missed", i, 0, 1);   // deadline already in the past
            continue;
        }
        uint32_t spins = 0;
        while (!s_isr_count && ++spins < SPIN_LIMIT) {}
        uint32_t elapsed = timing_net(t0, s_isr_cycles);
        g_bench_samples[i] = elapsed > s_target_cycles ? elapsed - s_target_cycles : 0;
    }
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "alarm_latency", UNIT_CYCLES, 1, &s);
    rec_text(ctx, "alarm_note", "cycles between the requested %lu us deadline and the first "
                                "instruction of the handler", (unsigned long)delay_us);
    guard_keepalive();

    // ---- The same with DMA traffic ----
    int ch = dma_claim_unused_channel(false);
    if (ch >= 0 && bench_ram_size() >= 64u * 1024u) {
        dma_channel_config c = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, true);
        for (uint32_t i = 0; i < SAMPLES; i++) {
            dma_channel_configure(ch, &c, bench_ram() + 32u * 1024u, bench_ram(), 4096, true);
            s_isr_count = 0;
            uint32_t t0 = timing_now();
            hardware_alarm_set_target((uint)s_alarm, make_timeout_time_us(delay_us));
            uint32_t spins = 0;
            while (!s_isr_count && ++spins < SPIN_LIMIT) {}
            uint32_t elapsed = timing_net(t0, s_isr_cycles);
            g_bench_samples[i] = elapsed > s_target_cycles ? elapsed - s_target_cycles : 0;
            dma_channel_abort(ch);
        }
        stats_from_samples(g_bench_samples, SAMPLES, &s);
        rec_stats(ctx, "alarm_latency_with_dma", UNIT_CYCLES, 1, &s);
        dma_channel_unclaim(ch);
    }

    hardware_alarm_set_callback((uint)s_alarm, NULL);
    hardware_alarm_unclaim((uint)s_alarm);
    s_alarm = -1;
    return TR_PASS;
}

// ---- Interrupt storm ------------------------------------------------------------------------

static volatile bool s_storm_run;
static repeating_timer_t s_storm_timer;

static bool storm_cb(repeating_timer_t *t) {
    (void)t;
    s_isr_count++;
    return s_storm_run;
}

// The work the foreground keeps doing while interrupts rain on it.
static uint32_t __not_in_flash_func(foreground)(uint32_t n) {
    uint32_t a = n | 1u;
    for (uint32_t i = 0; i < 64; i++) {
        a = a * 5u + 1u;
        __asm volatile("" : "+r"(a));
    }
    return a;
}

test_result_t test_irq_storm(test_ctx_t *ctx) {
    stats_t quiet, storm;
    uint32_t (*volatile fn)(uint32_t) = foreground;

    for (uint32_t i = 0; i < SAMPLES; i++) {
        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = timing_now();
        uint32_t r = fn(i);
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    stats_from_samples(g_bench_samples, SAMPLES, &quiet);
    rec_stats(ctx, "foreground_quiet", UNIT_CYCLES, 64, &quiet);
    guard_keepalive();

    // 50 kHz of interrupts: heavy, but a plausible worst case for a busy firmware.
    s_isr_count = 0;
    s_storm_run = true;
    if (!add_repeating_timer_us(-20, storm_cb, NULL, &s_storm_timer)) {
        rec_text(ctx, "storm", "could not start the repeating timer");
        return TR_PASS;
    }
    for (uint32_t i = 0; i < SAMPLES; i++) {
        uint32_t t0 = timing_now();
        uint32_t r = fn(i);
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    s_storm_run = false;
    cancel_repeating_timer(&s_storm_timer);
    stats_from_samples(g_bench_samples, SAMPLES, &storm);
    rec_stats(ctx, "foreground_under_storm", UNIT_CYCLES, 64, &storm);
    rec_value(ctx, "storm_interrupts", UNIT_COUNT, s_isr_count);
    rec_value(ctx, "storm_worst_extra_cycles", UNIT_CYCLES, (int64_t)storm.max - (int64_t)quiet.max);
    rec_text(ctx, "note", "a 50 kHz repeating timer with a handler in flash; the worst case is what "
                          "a bus-servicing loop would have to survive");
    return TR_PASS;
}
