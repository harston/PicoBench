/*
 * Shared helpers for measurement tests.
 *
 * Buffers: one sample buffer reused by every test (tests run one at a time), plus the
 * unused heap region, which is by far the largest free block of SRAM and is where the
 * memory tests and benchmarks work. Nothing allocates from the heap in this firmware
 * (no malloc anywhere, TinyUSB buffers are static), so the whole region is free.
 *
 * Measurement rules (see CLAUDE.md):
 *   - the loop under test runs from SRAM (__not_in_flash_func) unless XIP is the target,
 *   - interrupts are disabled around a sample,
 *   - the cycle-counter read overhead is subtracted (timing_net).
 */

#ifndef PICOBENCH_BENCH_H
#define PICOBENCH_BENCH_H

#include <stdint.h>

#include "hardware/sync.h"

#if PICO_RP2040
#define BENCH_SAMPLES_MAX 8192u
#else
#define BENCH_SAMPLES_MAX 16384u
#endif

extern uint32_t g_bench_samples[BENCH_SAMPLES_MAX];

// Streaming-statistics bins (exact per-cycle histogram below this value).
#define BENCH_STREAM_BINS 1024u
extern uint32_t g_bench_stream_bins[BENCH_STREAM_BINS];

// Free SRAM (the unused heap), 32-byte aligned. Valid for the whole run.
uint8_t *bench_ram(void);
uint32_t bench_ram_size(void);

/*
 * SRAM bank groups. Both chips stripe their large banks with 4-byte granularity, so
 * address bits [3:2] select the bank inside a group: a 4-byte stride rotates through
 * all four banks, a 16-byte stride keeps hitting the same one.
 *   RP2040: group 0 = SRAM0-3  (0x20000000, 256 KB)
 *   RP2350: group 0 = SRAM0-3  (0x20000000, 256 KB), group 1 = SRAM4-7 (0x20040000, 256 KB)
 * The 4 KB scratch banks hold the stacks and are not touched.
 */
#if PICO_RP2040
#define BENCH_BANK_GROUPS 1u
#else
#define BENCH_BANK_GROUPS 2u
#endif
#define BENCH_STRIPE_BYTES 4u
#define BENCH_BANKS_PER_GROUP 4u

// Free part of bank group `g` inside bench_ram(), or size 0 if none.
uint8_t *bench_group_ram(unsigned g, uint32_t *size);
const char *bench_group_name(unsigned g);

// A deterministic PRNG, so runs are comparable (xorshift32).
static inline uint32_t bench_rand(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#endif
