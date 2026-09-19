/*
 * Latency statistics.
 *
 * Two collectors:
 *  - stats_from_samples(): exact percentiles from a sample buffer (sorted in place).
 *  - stats_stream_*():      unbounded sample count (millions of hot-path iterations).
 *    Exact per-cycle histogram below `lin_bins`, log2 buckets above it. Percentiles
 *    that land in a log2 bucket are reported as the bucket's upper bound (clamped to
 *    max), which is conservative, and the result is flagged STATS_APPROX.
 *
 * Percentiles use the nearest-rank method. p999 is the 99.9th percentile.
 */

#ifndef PICOBENCH_STATS_H
#define PICOBENCH_STATS_H

#include <stdint.h>
#include <stdbool.h>

#define STATS_APPROX 0x1u      // some percentile came from a log2 bucket

typedef struct {
    uint32_t n;
    uint32_t min;
    uint32_t p50;
    uint32_t p95;
    uint32_t p99;
    uint32_t p999;
    uint32_t max;
    uint32_t flags;
    uint64_t sum;
} stats_t;

#define STATS_LOG2_BUCKETS 33  // bucket b holds values with bit length b (0 -> value 0)

// Sorts `samples` in place (heapsort: no extra memory, deterministic time).
void stats_from_samples(uint32_t *samples, uint32_t n, stats_t *out);

// log2 histogram of arbitrary samples (order does not matter).
void stats_log2_hist(const uint32_t *samples, uint32_t n, uint32_t hist[STATS_LOG2_BUCKETS]);

typedef struct {
    uint32_t *lin;            // lin[v] counts samples equal to v, for v < lin_bins
    uint32_t lin_bins;
    uint32_t log2[STATS_LOG2_BUCKETS];  // samples >= lin_bins only
    uint32_t n;
    uint32_t min;
    uint32_t max;
    uint64_t sum;
} stats_stream_t;

void stats_stream_init(stats_stream_t *s, uint32_t *lin_buf, uint32_t lin_bins);
void stats_stream_finish(const stats_stream_t *s, stats_t *out);

static inline uint32_t stats_bitlen(uint32_t v) {
    return v ? 32u - (uint32_t)__builtin_clz(v) : 0u;
}

static inline void stats_stream_add(stats_stream_t *s, uint32_t v) {
    if (v < s->lin_bins) s->lin[v]++;
    else s->log2[stats_bitlen(v)]++;
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    s->sum += v;
    s->n++;
}

#endif
