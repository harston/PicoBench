/*
 * Shared helpers for tests: one sample buffer reused by every test (tests run one at
 * a time), and interrupt masking around measurement loops.
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

#endif
