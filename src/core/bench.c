#include "core/bench.h"

#include "pico.h"
#include "core/timing.h"
#include "hardware/xip_cache.h"

uint32_t g_bench_samples[BENCH_SAMPLES_MAX] __attribute__((aligned(32)));
uint32_t g_bench_stream_bins[BENCH_STREAM_BINS];

// The heap: free because nothing in this firmware allocates.
extern char __end__, __HeapLimit;

uint8_t *bench_ram(void) {
    uintptr_t start = ((uintptr_t)&__end__ + 31u) & ~(uintptr_t)31u;
    return (uint8_t *)start;
}

uint32_t bench_ram_size(void) {
    uintptr_t start = (uintptr_t)bench_ram();
    uintptr_t end = (uintptr_t)&__HeapLimit;
    return end > start ? (uint32_t)(end - start) : 0;
}

#define GROUP0_BASE 0x20000000u
#define GROUP0_END  0x20040000u
#define GROUP1_BASE 0x20040000u
#define GROUP1_END  0x20080000u

uint8_t *bench_group_ram(unsigned g, uint32_t *size) {
    uintptr_t lo = (uintptr_t)bench_ram();
    uintptr_t hi = lo + bench_ram_size();
    uintptr_t glo = g == 0 ? GROUP0_BASE : GROUP1_BASE;
    uintptr_t ghi = g == 0 ? GROUP0_END : GROUP1_END;
    if (lo < glo) lo = glo;
    if (hi > ghi) hi = ghi;
    *size = hi > lo ? (uint32_t)(hi - lo) : 0;
    return (uint8_t *)lo;
}

unsigned bench_group_of(const void *p) {
#if PICO_RP2040
    (void)p;
    return 0;
#else
    return (uintptr_t)p >= GROUP1_BASE ? 1u : 0u;
#endif
}

uint8_t *bench_group_block(unsigned g, uint32_t need, uint32_t keep_words, const char **source) {
    if (g >= BENCH_BANK_GROUPS) return NULL;
    uint32_t size;
    uint8_t *heap = bench_group_ram(g, &size);
    if (size >= need) {
        *source = "heap";
        return heap;
    }
    uintptr_t glo = g == 0 ? GROUP0_BASE : GROUP1_BASE;
    uintptr_t ghi = g == 0 ? GROUP0_END : GROUP1_END;
    uintptr_t lo = (uintptr_t)&g_bench_samples[keep_words < BENCH_SAMPLES_MAX ? keep_words : BENCH_SAMPLES_MAX];
    uintptr_t hi = (uintptr_t)&g_bench_samples[BENCH_SAMPLES_MAX];
    if (lo < glo) lo = glo;
    if (hi > ghi) hi = ghi;
    if (hi < lo + need) return NULL;
    uintptr_t start = (hi - need) & ~(uintptr_t)31u;
    if (start < lo) return NULL;
    *source = "sample buffer";
    return (uint8_t *)start;
}

const char *bench_group_name(unsigned g) {
#if PICO_RP2040
    (void)g;
    return "SRAM0-3 (striped)";
#else
    return g == 0 ? "SRAM0-3 (striped)" : "SRAM4-7 (striped)";
#endif
}

static volatile uint32_t s_cold_sink;

void __no_inline_not_in_flash_func(bench_cold_reads)(const uint8_t *base, uint32_t stride, uint32_t n,
                                                     uint32_t *out) {
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < n; i++) {
        xip_cache_invalidate_all();     // in SRAM as well (SDK: __no_inline_not_in_flash_func)
        const volatile uint32_t *p = (const volatile uint32_t *)(base + (size_t)i * stride);
        __asm volatile("" : "+r"(p));
        uint32_t t0 = timing_now();
        uint32_t v = *p;
        uint32_t t1 = timing_now();
        out[i] = timing_net(t0, t1);
        s_cold_sink += v;
    }
    restore_interrupts(irq);
}
