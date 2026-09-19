/*
 * Infrastructure self-checks. These validate the measurement tools themselves, so
 * that every later result can be trusted: the cycle counter must agree with the
 * independent 1 MHz timer, and clk_sys must be what was requested.
 */

#include <stdlib.h>

#include "tests/tests.h"
#include "core/bench.h"
#include "core/clocks_ctl.h"
#include "core/stats.h"
#include "core/timing.h"
#include "hardware/clocks.h"
#include "pico/time.h"

// Deviation of `measured` from `expected` in parts per million.
static int32_t ppm(uint32_t measured, uint32_t expected) {
    if (expected == 0) return INT32_MAX;
    return (int32_t)(((int64_t)measured - (int64_t)expected) * 1000000 / expected);
}

test_result_t test_timing_counter(test_ctx_t *ctx) {
    rec_text(ctx, "counter", "%s", TIMING_COUNTER_NAME);
    rec_value(ctx, "counter_bits", UNIT_COUNT, TIMING_COUNTER_BITS);
    rec_value(ctx, "read_overhead", UNIT_CYCLES, timing_overhead());

    // Cost of back-to-back reads, IRQs off: shows read jitter of the counter itself.
    const uint32_t n = 4096;
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t0 = timing_now();
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_elapsed(t0, t1);
    }
    restore_interrupts(irq);
    stats_t s;
    stats_from_samples(g_bench_samples, n, &s);
    rec_stats(ctx, "back_to_back_read", UNIT_CYCLES, 0, &s);

    // Cross-check against the 1 MHz timer over 10 ms. On RP2040 the 24-bit SysTick
    // covers ~126 ms at 133 MHz, so 10 ms is safely inside one counter period.
    irq = save_and_disable_interrupts();
    uint64_t u0 = time_us_64();
    while (time_us_64() == u0) {}
    u0 = time_us_64();
    uint32_t c0 = timing_now();
    uint64_t u1;
    do { u1 = time_us_64(); } while (u1 - u0 < 10000);
    uint32_t c1 = timing_now();
    restore_interrupts(irq);

    uint32_t cycles = timing_elapsed(c0, c1);
    uint32_t us = (uint32_t)(u1 - u0);
    uint32_t counter_khz = (uint32_t)((uint64_t)cycles * 1000u / us);
    uint32_t cfg_khz = clock_get_hz(clk_sys) / 1000u;
    int32_t dev = ppm(counter_khz, cfg_khz);
    rec_value(ctx, "window_us", UNIT_US, us);
    rec_value(ctx, "window_cycles", UNIT_CYCLES, cycles);
    rec_value(ctx, "counter_khz", UNIT_KHZ, counter_khz);
    rec_value(ctx, "configured_khz", UNIT_KHZ, cfg_khz);
    rec_value(ctx, "deviation", UNIT_PPM, dev);

    // Both sides derive from the same crystal, so they must agree to within the
    // timer's 1 us resolution over 10 ms (100 ppm) plus a small margin.
    if (abs(dev) > 500) {
        rec_error(ctx, "counter_vs_timer", 0, cfg_khz, counter_khz);
        return TR_FAIL;
    }
    return TR_PASS;
}

test_result_t test_clocks_readback(test_ctx_t *ctx) {
    clock_readback_t rb;
    clocks_readback(&rb);
    uint32_t req = clocks_op_khz(ctx->op);
    int32_t dev = ppm(rb.sys_khz, req);
    rec_value(ctx, "requested_khz", UNIT_KHZ, req);
    rec_value(ctx, "measured_sys_khz", UNIT_KHZ, rb.sys_khz);
    rec_value(ctx, "pll_sys_out_khz", UNIT_KHZ, rb.sys_pll.out_khz);
    rec_value(ctx, "sys_deviation", UNIT_PPM, dev);
    rec_value(ctx, "flash_if_div", UNIT_COUNT, rb.flash_if_div);

    test_result_t res = TR_PASS;
    // Frequency counter resolution is ~1 kHz; allow 0.2 % (e.g. 300 kHz at 150 MHz).
    if (abs(dev) > 2000) {
        rec_error(ctx, "sys_khz_measured", 0, req, rb.sys_khz);
        res = TR_FAIL;
    }
    if (rb.sys_pll.out_khz != req && (rb.sys_ctrl & 1u)) {
        rec_error(ctx, "pll_sys_out_khz", 0, req, rb.sys_pll.out_khz);
        res = TR_FAIL;
    }
    if (!rb.sys_pll.locked) {
        rec_error(ctx, "pll_sys_lock", 0, 1, 0);
        res = TR_FAIL;
    }
    return res;
}

// Runs from SRAM so the result does not depend on XIP cache state.
static void __not_in_flash_func(empty_loop)(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        __asm volatile("" ::: "memory");
    }
}

test_result_t test_cpu_loop_smoke(test_ctx_t *ctx) {
    const uint32_t iters = 100;
    const uint32_t n = 2048;
    empty_loop(iters);   // warm-up
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t0 = timing_now();
        empty_loop(iters);
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_net(t0, t1);
    }
    restore_interrupts(irq);
    stats_t s;
    stats_from_samples(g_bench_samples, n, &s);
    rec_stats(ctx, "loop100", UNIT_CYCLES, iters, &s);
    // Code in SRAM with IRQs off must be deterministic: flag any spread as suspicious
    // but not as a failure (bus contention from DMA/USB is legitimately possible).
    rec_value(ctx, "spread_cycles", UNIT_CYCLES, (int64_t)s.max - s.min);
    return TR_PASS;
}

test_result_t test_debug_crash(test_ctx_t *ctx) {
    (void)ctx;
    (void)save_and_disable_interrupts();
    for (;;) {
        __asm volatile("" ::: "memory");
    }
}
