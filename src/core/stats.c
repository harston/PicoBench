#include "core/stats.h"

#include <string.h>

// Percentiles in parts per 10000.
#define P50  5000u
#define P95  9500u
#define P99  9900u
#define P999 9990u

// Nearest-rank index (0-based) for percentile p/10000 of n samples.
static uint32_t rank_index(uint32_t n, uint32_t p) {
    uint64_t r = ((uint64_t)p * n + 9999u) / 10000u;   // ceil(p * n)
    if (r == 0) r = 1;
    return (uint32_t)(r - 1);
}

static void sift_down(uint32_t *a, uint32_t start, uint32_t end) {
    uint32_t root = start;
    for (;;) {
        uint32_t child = 2 * root + 1;
        if (child >= end) return;
        if (child + 1 < end && a[child] < a[child + 1]) child++;
        if (a[root] >= a[child]) return;
        uint32_t t = a[root]; a[root] = a[child]; a[child] = t;
        root = child;
    }
}

static void heapsort_u32(uint32_t *a, uint32_t n) {
    if (n < 2) return;
    for (uint32_t i = n / 2; i-- > 0;) sift_down(a, i, n);
    for (uint32_t end = n - 1; end > 0; end--) {
        uint32_t t = a[0]; a[0] = a[end]; a[end] = t;
        sift_down(a, 0, end);
    }
}

void stats_from_samples(uint32_t *samples, uint32_t n, stats_t *out) {
    memset(out, 0, sizeof(*out));
    if (n == 0) return;
    heapsort_u32(samples, n);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += samples[i];
    out->n = n;
    out->sum = sum;
    out->min = samples[0];
    out->max = samples[n - 1];
    out->p50 = samples[rank_index(n, P50)];
    out->p95 = samples[rank_index(n, P95)];
    out->p99 = samples[rank_index(n, P99)];
    out->p999 = samples[rank_index(n, P999)];
}

void stats_log2_hist(const uint32_t *samples, uint32_t n, uint32_t hist[STATS_LOG2_BUCKETS]) {
    memset(hist, 0, STATS_LOG2_BUCKETS * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) hist[stats_bitlen(samples[i])]++;
}

void stats_stream_init(stats_stream_t *s, uint32_t *lin_buf, uint32_t lin_bins) {
    memset(s, 0, sizeof(*s));
    s->lin = lin_buf;
    s->lin_bins = lin_bins;
    s->min = UINT32_MAX;
    memset(lin_buf, 0, lin_bins * sizeof(uint32_t));
}

// Value at 0-based rank `idx` in the streamed distribution.
static uint32_t stream_value_at(const stats_stream_t *s, uint32_t idx, uint32_t *flags) {
    uint32_t seen = 0;
    for (uint32_t v = 0; v < s->lin_bins; v++) {
        seen += s->lin[v];
        if (seen > idx) return v;
    }
    for (uint32_t b = 0; b < STATS_LOG2_BUCKETS; b++) {
        seen += s->log2[b];
        if (seen > idx) {
            *flags |= STATS_APPROX;
            uint32_t upper = b >= 32 ? UINT32_MAX : (1u << b) - 1u;
            return upper < s->max ? upper : s->max;
        }
    }
    return s->max;
}

void stats_stream_finish(const stats_stream_t *s, stats_t *out) {
    memset(out, 0, sizeof(*out));
    if (s->n == 0) return;
    out->n = s->n;
    out->sum = s->sum;
    out->min = s->min;
    out->max = s->max;
    out->p50 = stream_value_at(s, rank_index(s->n, P50), &out->flags);
    out->p95 = stream_value_at(s, rank_index(s->n, P95), &out->flags);
    out->p99 = stream_value_at(s, rank_index(s->n, P99), &out->flags);
    out->p999 = stream_value_at(s, rank_index(s->n, P999), &out->flags);
}
