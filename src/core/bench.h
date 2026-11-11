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

// Bank group of an SRAM address (0 or 1; always 0 on the RP2040).
unsigned bench_group_of(const void *p);

// A 32-byte aligned block of `need` bytes wholly inside bank group `g`, for tests that
// must place memory in a particular group: the free heap part of that group when it is
// large enough, otherwise the part of g_bench_samples that lies in that group. The
// fallback exists for the RP2350, where the firmware's own data fills group 0 and the
// sample buffer is the only free block left there (all of the heap is in group 1).
// The block is taken from the END of the sample buffer, so the caller may keep using
// g_bench_samples[0 .. keep_words) meanwhile - and must not use the rest.
// *source names what was used ("heap" / "sample buffer"). NULL if neither fits.
uint8_t *bench_group_block(unsigned g, uint32_t need, uint32_t keep_words, const char **source);
const char *bench_group_name(unsigned g);

// Cost of one XIP cache miss: n single 32-bit reads at base + i * stride, the whole
// XIP cache invalidated before each one, cycles stored in out[0 .. n).
// Runs entirely from SRAM with interrupts off: after the invalidate, code fetched from
// flash would miss too and land in or just before the timed read - which is exactly
// what made qmi.conflict and the first flash.cache / psram.bench miss figures depend on
// where the linker put the loop. base must be a cached XIP address (flash or PSRAM).
void bench_cold_reads(const uint8_t *base, uint32_t stride, uint32_t n, uint32_t *out);

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
