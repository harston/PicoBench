/*
 * SIO and GPIO timing.
 *
 * Everything here is *internal* timing: the cost of the register operations and the
 * delay until a driven level is visible again in the input register. The electrical
 * side - rise time, PCB propagation, when the voltage crosses the Atari input
 * threshold - cannot be measured from inside the chip and is not claimed here.
 *
 * Driving a pin needs a pin that is free. Before driving anything the test checks that
 * the candidate floats: with the internal pull-up it must read high and with the
 * pull-down low. A pin held by anything external fails that check and is skipped, so
 * the test never fights another driver.
 */

#include "tests/tests.h"

#include <stdio.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/platform.h"
#include "core/sequencer.h"
#include "core/stats.h"
#include "core/timing.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "results/record.h"

#define SAMPLES 512u

static volatile uint32_t s_sink;

// Candidate pins, in order: plain header pins on every supported board. GPIO25 is the
// LED and is never used here.
static const uint8_t s_candidates[] = {28, 27, 26, 22, 21, 20};

// A pin is usable if it floats: pull-up reads 1, pull-down reads 0.
static bool pin_is_free(uint8_t pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
    busy_wait_us(50);
    bool high = gpio_get(pin);
    gpio_pull_down(pin);
    busy_wait_us(50);
    bool low = gpio_get(pin);
    gpio_disable_pulls(pin);
    return high && !low;
}

static int find_free_pin(void) {
    for (unsigned i = 0; i < sizeof(s_candidates); i++) {
        uint8_t p = s_candidates[i];
        if (p >= NUM_BANK0_GPIOS) continue;
        if (pin_is_free(p)) return p;
    }
    return -1;
}

// ---- Register operation costs ------------------------------------------------------------

typedef uint32_t (*op_fn)(uint32_t mask);

#define OP_SAMPLES 256u

static void sample_op(op_fn fn, uint32_t mask, stats_t *out) {
    op_fn volatile vfn = fn;
    s_sink += vfn(mask);
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < OP_SAMPLES; i++) {
        uint32_t m = mask;
        __asm volatile("" : "+r"(m));
        uint32_t t0 = timing_now();
        uint32_t r = vfn(m);
        uint32_t t1 = timing_now();
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += r;
    }
    restore_interrupts(irq);
    stats_from_samples(g_bench_samples, OP_SAMPLES, out);
}

// Each of these is one SIO register access, the operation a bus-servicing loop uses.
static uint32_t __not_in_flash_func(op_set)(uint32_t mask) { sio_hw->gpio_set = mask; return mask; }
static uint32_t __not_in_flash_func(op_clr)(uint32_t mask) { sio_hw->gpio_clr = mask; return mask; }
static uint32_t __not_in_flash_func(op_xor)(uint32_t mask) { sio_hw->gpio_togl = mask; return mask; }
static uint32_t __not_in_flash_func(op_out)(uint32_t mask) { sio_hw->gpio_out = mask; return mask; }
static uint32_t __not_in_flash_func(op_in)(uint32_t mask) { (void)mask; return sio_hw->gpio_in; }
static uint32_t __not_in_flash_func(op_oe_set)(uint32_t mask) { sio_hw->gpio_oe_set = mask; return mask; }
static uint32_t __not_in_flash_func(op_oe_clr)(uint32_t mask) { sio_hw->gpio_oe_clr = mask; return mask; }

static void record_op(test_ctx_t *ctx, const char *label, op_fn fn, uint32_t mask) {
    stats_t s;
    sample_op(fn, mask, &s);
    rec_stats(ctx, label, UNIT_CYCLES, 1, &s);
    guard_keepalive();
}

// ---- Output turnaround -------------------------------------------------------------------

// Cycles from writing the output register until the new level is readable back in the
// input register of the same pin. This is the internal part of "put a byte on the bus".
#define TURNAROUND_SPIN_LIMIT 10000u

// Returns 0 if the level never appeared (pin held by something external after all).
static uint32_t __no_inline_not_in_flash_func(measure_turnaround)(uint32_t mask, bool level) {
    uint32_t irq = save_and_disable_interrupts();
    if (level) sio_hw->gpio_clr = mask; else sio_hw->gpio_set = mask;   // start from the other level
    uint32_t spins = 0;
    while (((sio_hw->gpio_in & mask) != 0) == level && ++spins < TURNAROUND_SPIN_LIMIT) {}
    uint32_t t0 = timing_now();
    if (level) sio_hw->gpio_set = mask; else sio_hw->gpio_clr = mask;
    spins = 0;
    while (((sio_hw->gpio_in & mask) != 0) != level && ++spins < TURNAROUND_SPIN_LIMIT) {}
    uint32_t t1 = timing_now();
    restore_interrupts(irq);
    return spins < TURNAROUND_SPIN_LIMIT ? timing_net(t0, t1) : 0;
}

test_result_t test_gpio_sio(test_ctx_t *ctx) {
    rec_value(ctx, "gpio_count", UNIT_COUNT, NUM_BANK0_GPIOS);
    rec_text(ctx, "scope", "internal timing only: register cost and input-register turnaround. "
                           "Rise time, PCB propagation and input thresholds are NOT measured.");

    // Register costs do not need a free pin: mask 0 touches no pad.
    record_op(ctx, "sio_gpio_set", op_set, 0);
    record_op(ctx, "sio_gpio_clr", op_clr, 0);
    record_op(ctx, "sio_gpio_xor", op_xor, 0);
    record_op(ctx, "sio_gpio_out_write", op_out, 0);
    record_op(ctx, "sio_gpio_in_read", op_in, 0);
    record_op(ctx, "sio_oe_set", op_oe_set, 0);
    record_op(ctx, "sio_oe_clr", op_oe_clr, 0);

    int pin = find_free_pin();
    if (pin < 0) {
        rec_text(ctx, "turnaround", "no free pin found among GPIO 28/27/26/22/21/20 "
                                    "(all were held by something external): not measured");
        return TR_PASS;
    }
    rec_value(ctx, "turnaround_pin", UNIT_COUNT, pin);
    uint32_t mask = 1u << pin;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);

    uint32_t timeouts = 0;
    for (uint32_t i = 0; i < SAMPLES; i++) {
        g_bench_samples[i] = measure_turnaround(mask, (i & 1u) != 0);
        if (g_bench_samples[i] == 0) timeouts++;
    }
    stats_t s;
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "output_to_input_turnaround", UNIT_CYCLES, 1, &s);
    if (timeouts) {
        rec_error(ctx, "turnaround_timeout", (uint32_t)pin, 0, timeouts);
        rec_text(ctx, "turnaround_warning", "%lu of %lu attempts never saw the driven level",
                 (unsigned long)timeouts, (unsigned long)SAMPLES);
    }

    // Direction change: input -> output -> input, the cost of releasing the bus.
    for (uint32_t i = 0; i < OP_SAMPLES; i++) {
        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = timing_now();
        sio_hw->gpio_oe_set = mask;
        sio_hw->gpio_oe_clr = mask;
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        g_bench_samples[i] = timing_net(t0, t1);
    }
    stats_from_samples(g_bench_samples, OP_SAMPLES, &s);
    rec_stats(ctx, "direction_out_then_in", UNIT_CYCLES, 2, &s);

    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);
    rec_text(ctx, "pin_restored", "GPIO%d left as a floating input", pin);
    return TR_PASS;
}
