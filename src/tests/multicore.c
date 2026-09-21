/*
 * The second core: how long it takes to start, how fast the two cores can exchange a
 * word, what a spinlock costs, and what core 1 doing memory work does to core 0.
 *
 * The last one is the question behind a cartridge design that puts the bus handling on
 * one core and everything else on the other.
 *
 * Core 1 runs entirely from SRAM: code in flash would add XIP traffic of its own and
 * the contention numbers would measure the wrong thing.
 */

#include "tests/tests.h"

#include <stdio.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "results/record.h"

#define SAMPLES 256u

enum { CMD_IDLE = 0, CMD_ECHO, CMD_HAMMER_G0, CMD_HAMMER_G1, CMD_EXIT };

static volatile uint32_t s_cmd;
static volatile uint32_t s_core1_ready;
static volatile uint32_t s_hammer_count;
static uint8_t *s_hammer_buf[2];
static uint32_t s_hammer_words[2];
static volatile uint32_t s_sink;

static void __no_inline_not_in_flash_func(core1_main)(void) {
    timing_init();                       // the cycle counter is per core
    s_core1_ready = 1;
    for (;;) {
        uint32_t cmd = s_cmd;
        if (cmd == CMD_EXIT) break;
        if (cmd == CMD_ECHO) {
            if (multicore_fifo_rvalid()) multicore_fifo_push_blocking(multicore_fifo_pop_blocking());
        } else if (cmd == CMD_HAMMER_G0 || cmd == CMD_HAMMER_G1) {
            unsigned g = cmd == CMD_HAMMER_G0 ? 0 : 1;
            const uint32_t *p = (const uint32_t *)s_hammer_buf[g];
            uint32_t n = s_hammer_words[g], acc = 0;
            for (uint32_t i = 0; i < n; i += 8) {
                acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
            }
            s_sink += acc;
            s_hammer_count++;
        }
    }
    s_core1_ready = 0;
}

// Core 0's measurement loop, the same shape as the SRAM throughput test.
static uint32_t __not_in_flash_func(read_block)(const void *base, uint32_t bytes) {
    const uint32_t *p = base;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return acc;
}

static void sample_read(const void *base, uint32_t bytes, stats_t *out) {
    uint32_t (*volatile fn)(const void *, uint32_t) = read_block;
    for (uint32_t i = 0; i < SAMPLES; i++) {
        const void *b = base;
        __asm volatile("" : "+r"(b));
        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = timing_now();
        uint32_t r = fn(b, bytes);
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    stats_from_samples(g_bench_samples, SAMPLES, out);
}

test_result_t test_multicore(test_ctx_t *ctx) {
    uint32_t g0_size, g1_size;
    uint8_t *g0 = bench_group_ram(0, &g0_size);
    uint8_t *g1 = bench_group_ram(1, &g1_size);
    if (g0_size < 64u * 1024u) return TR_NA;

    s_hammer_buf[0] = g0 + 16u * 1024u;
    s_hammer_words[0] = 4096;
    s_hammer_buf[1] = g1_size >= 32u * 1024u ? g1 : s_hammer_buf[0];
    s_hammer_words[1] = 4096;

    // ---- Start core 1 ----
    s_cmd = CMD_IDLE;
    s_core1_ready = 0;
    uint32_t t0 = timing_now();
    multicore_launch_core1(core1_main);
    uint32_t spins = 0;
    while (!s_core1_ready && ++spins < 100000000u) {}
    uint32_t launch = timing_net(t0, timing_now());
    if (!s_core1_ready) {
        rec_error(ctx, "core1_launch", 0, 1, 0);
        return TR_FAIL;
    }
    rec_value(ctx, "core1_launch", UNIT_CYCLES, launch);
    rec_value(ctx, "core1_launch_us", UNIT_US, launch / (ctx->clk_khz / 1000u));
    guard_keepalive();

    // ---- Inter-core FIFO round trip ----
    s_cmd = CMD_ECHO;
    for (uint32_t i = 0; i < SAMPLES; i++) {
        uint32_t irq = save_and_disable_interrupts();
        uint32_t a = timing_now();
        multicore_fifo_push_blocking(i);
        uint32_t spins = 0;
        while (!multicore_fifo_rvalid() && ++spins < 10000000u) {}
        uint32_t v = multicore_fifo_rvalid() ? multicore_fifo_pop_blocking() : 0;
        uint32_t b = timing_now();
        restore_interrupts(irq);
        g_bench_samples[i] = timing_net(a, b);
        s_sink += v;
    }
    stats_t s;
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "fifo_round_trip", UNIT_CYCLES, 1, &s);
    s_cmd = CMD_IDLE;
    guard_keepalive();

    // ---- Spinlock ----
    int lock_num = spin_lock_claim_unused(false);
    if (lock_num >= 0) {
        spin_lock_t *lock = spin_lock_instance((uint)lock_num);
        for (uint32_t i = 0; i < SAMPLES; i++) {
            uint32_t irq = save_and_disable_interrupts();
            uint32_t a = timing_now();
            uint32_t save = spin_lock_blocking(lock);
            spin_unlock(lock, save);
            uint32_t b = timing_now();
            restore_interrupts(irq);
            g_bench_samples[i] = timing_net(a, b);
        }
        stats_from_samples(g_bench_samples, SAMPLES, &s);
        rec_stats(ctx, "spinlock_acquire_release", UNIT_CYCLES, 1, &s);
        spin_lock_unclaim((uint)lock_num);
    }
    guard_keepalive();

    // ---- Contention: core 1 reading memory while core 0 measures ----
    stats_t quiet, same, other;
    sample_read(g0, 4096, &quiet);
    rec_stats(ctx, "core0_read_quiet", UNIT_CYCLES, 4096, &quiet);

    s_hammer_count = 0;
    s_cmd = CMD_HAMMER_G0;
    sample_read(g0, 4096, &same);
    uint32_t loops_same = s_hammer_count;
    s_cmd = CMD_IDLE;
    rec_stats(ctx, "core0_read_core1_same_group", UNIT_CYCLES, 4096, &same);
    rec_value(ctx, "slowdown_same_group_percent", UNIT_COUNT,
              quiet.p50 ? (int64_t)((uint64_t)same.p50 * 100u / quiet.p50) - 100 : 0);
    rec_value(ctx, "core1_loops_same_group", UNIT_COUNT, loops_same);
    guard_keepalive();

    if (g1_size >= 32u * 1024u) {
        s_hammer_count = 0;
        s_cmd = CMD_HAMMER_G1;
        sample_read(g0, 4096, &other);
        uint32_t loops_other = s_hammer_count;
        s_cmd = CMD_IDLE;
        rec_stats(ctx, "core0_read_core1_other_group", UNIT_CYCLES, 4096, &other);
        rec_value(ctx, "slowdown_other_group_percent", UNIT_COUNT,
                  quiet.p50 ? (int64_t)((uint64_t)other.p50 * 100u / quiet.p50) - 100 : 0);
        rec_value(ctx, "core1_loops_other_group", UNIT_COUNT, loops_other);
    } else {
        rec_text(ctx, "other_group", "this chip has only one large bank group");
    }

    // ---- Park core 1 again ----
    s_cmd = CMD_EXIT;
    spins = 0;
    while (s_core1_ready && ++spins < 10000000u) {}
    multicore_reset_core1();
    rec_text(ctx, "core1_state", "stopped and reset; later tests run single-core again");
    return TR_PASS;
}
