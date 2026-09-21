/*
 * Cycle-accurate timing, one API for all three CPUs.
 *
 *   RP2350 Cortex-M33 : DWT CYCCNT, 32-bit, counts clk_sys cycles
 *   RP2350 Hazard3    : mcycle CSR (low 32 bits), counts clk_sys cycles
 *   RP2040 Cortex-M0+ : SysTick, 24-bit down-counter clocked from clk_sys
 *
 * Usage:
 *   uint32_t t0 = timing_now();
 *   ...code under test...
 *   uint32_t cyc = timing_elapsed(t0, timing_now()) - timing_overhead();
 *
 * Limits: a single measurement must stay below 2^TIMING_COUNTER_BITS cycles
 * (RP2040: 16.7M cycles = 126 ms at 133 MHz). Use time_us_64() for longer spans.
 *
 * The counters are per core: call timing_init() on every core that measures.
 */

#ifndef PICOBENCH_TIMING_H
#define PICOBENCH_TIMING_H

#include <stdint.h>
#include "pico.h"

#if PICO_RP2040
#include "hardware/structs/systick.h"
#define TIMING_COUNTER_BITS 24
#define TIMING_COUNTER_NAME "SysTick (24-bit)"
#elif defined(__riscv)
#include "hardware/riscv.h"
#define TIMING_COUNTER_BITS 32
#define TIMING_COUNTER_NAME "mcycle"
#else
#include "hardware/structs/m33.h"
#define TIMING_COUNTER_BITS 32
#define TIMING_COUNTER_NAME "DWT CYCCNT"
#endif

#define TIMING_COUNTER_MASK ((uint32_t)((1ull << TIMING_COUNTER_BITS) - 1))

// Enable the cycle counter on the calling core and calibrate the read overhead.
void timing_init(void);

// Cycles consumed by an empty timing_now()/timing_now() pair (minimum observed).
// Kept as a variable so subtracting it costs no call: a measurement loop in SRAM would
// otherwise call into flash on every iteration.
extern uint32_t g_timing_overhead;

static inline uint32_t timing_overhead(void) {
    return g_timing_overhead;
}

static __force_inline uint32_t timing_now(void) {
#if PICO_RP2040
    return systick_hw->cvr;
#elif defined(__riscv)
    return riscv_read_csr(mcycle);
#else
    return m33_hw->dwt_cyccnt;
#endif
}

static __force_inline uint32_t timing_elapsed(uint32_t start, uint32_t end) {
#if PICO_RP2040
    return (start - end) & TIMING_COUNTER_MASK;   // SysTick counts down
#else
    return end - start;
#endif
}

// Elapsed cycles with the read overhead removed (clamped at 0).
static __force_inline uint32_t timing_net(uint32_t start, uint32_t end) {
    uint32_t e = timing_elapsed(start, end);
    uint32_t o = timing_overhead();
    return e > o ? e - o : 0;
}

// Convert cycles to nanoseconds (x1000 for sub-ns resolution) at clk_sys in kHz.
static inline uint64_t timing_cycles_to_ps(uint64_t cycles, uint32_t clk_khz) {
    return clk_khz ? (cycles * 1000000000ull) / clk_khz : 0;
}

#endif
