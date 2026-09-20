#include "core/bench.h"

#include "pico.h"

uint32_t g_bench_samples[BENCH_SAMPLES_MAX];
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

const char *bench_group_name(unsigned g) {
#if PICO_RP2040
    (void)g;
    return "SRAM0-3 (striped)";
#else
    return g == 0 ? "SRAM0-3 (striped)" : "SRAM4-7 (striped)";
#endif
}
