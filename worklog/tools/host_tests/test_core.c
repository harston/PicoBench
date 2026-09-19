/*
 * Host-side unit tests for platform-independent firmware code
 * (src/core/stats.c, src/results/crc32.c).
 *
 * Build and run:  worklog/tools/host_tests/run.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/stats.h"
#include "results/crc32.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// Reference nearest-rank percentile.
static uint32_t ref_pct(const uint32_t *sorted, uint32_t n, double p) {
    uint64_t r = (uint64_t)((p * n) + 0.9999999);
    if (r == 0) r = 1;
    return sorted[r - 1];
}

static void test_crc32(void) {
    CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u);          // standard check value
    CHECK(crc32_update(0, "", 0) == 0);
    uint32_t split = crc32_update(crc32_update(0, "12345", 5), "6789", 4);
    CHECK(split == 0xCBF43926u);                                     // incremental == one-shot
}

static void test_stats_exact(void) {
    enum { N = 10007 };
    static uint32_t a[N], ref[N];
    srand(1);
    for (int i = 0; i < N; i++) a[i] = ref[i] = (uint32_t)(rand() % 5000) + (i % 97 == 0 ? 100000 : 0);
    qsort(ref, N, sizeof(uint32_t), cmp_u32);
    stats_t s;
    stats_from_samples(a, N, &s);
    CHECK(memcmp(a, ref, sizeof(a)) == 0);                           // heapsort is correct
    CHECK(s.n == N && s.min == ref[0] && s.max == ref[N - 1]);
    CHECK(s.p50 == ref_pct(ref, N, 0.50));
    CHECK(s.p95 == ref_pct(ref, N, 0.95));
    CHECK(s.p99 == ref_pct(ref, N, 0.99));
    CHECK(s.p999 == ref_pct(ref, N, 0.999));
    CHECK(s.flags == 0);

    uint32_t one = 42;
    stats_from_samples(&one, 1, &s);
    CHECK(s.min == 42 && s.p50 == 42 && s.p999 == 42 && s.max == 42);
}

static void test_stats_stream(void) {
    static uint32_t bins[1024];
    stats_stream_t st;
    stats_stream_init(&st, bins, 1024);
    enum { N = 100000 };
    static uint32_t all[N];
    srand(2);
    for (int i = 0; i < N; i++) {
        uint32_t v = (uint32_t)(rand() % 800);
        if (i % 500 == 0) v = 5000 + (uint32_t)(rand() % 3000);   // 0.2 % slow outliers
        all[i] = v;
        stats_stream_add(&st, v);
    }
    stats_t s;
    stats_stream_finish(&st, &s);
    qsort(all, N, sizeof(uint32_t), cmp_u32);
    CHECK(s.n == N && s.min == all[0] && s.max == all[N - 1]);
    CHECK(s.p50 == ref_pct(all, N, 0.50));                           // exact below lin_bins
    CHECK(s.p99 == ref_pct(all, N, 0.99));
    // p99.9 lands among the outliers: log2 bucket upper bound, clamped to max
    uint32_t exact = ref_pct(all, N, 0.999);
    CHECK((s.flags & STATS_APPROX) != 0);
    CHECK(s.p999 >= exact && s.p999 <= s.max);
}

int main(void) {
    test_crc32();
    test_stats_exact();
    test_stats_stream();
    printf("%s (%d failure(s))\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
