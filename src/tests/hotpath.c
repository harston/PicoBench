/*
 * Cartridge hot path: the one measurement the whole project exists for.
 *
 * Shape of the work, per bus cycle of the console:
 *     read the address bus  ->  decode bank and offset  ->  fetch the byte  ->
 *     put it on the data port
 *
 * What is real here and what is not:
 *   - the bus read is a real read of the SIO GPIO input register,
 *   - the data write is a real write of the SIO GPIO output register (the pads are
 *     left as inputs, so nothing is driven externally, but the bus cycle is real),
 *   - the addresses are synthetic: they come from a table in SRAM, because without
 *     a console there is nothing on the pins. The address is mixed with the value
 *     actually read from the pins through a mask that is zero at run time but unknown
 *     to the compiler, so the bus read cannot be optimised away and really is on the
 *     dependency path.
 *   - everything else - decode, the lookup and its memory, the cache state, the
 *     interrupt load and the DMA traffic - is exactly what the firmware would do.
 *
 * Every iteration is timed separately and fed to the streaming collector, so millions
 * of iterations give an exact distribution up to 1023 cycles, with log2 buckets above.
 * For this test the interesting number is not the median but p99.9 and max: one late
 * byte is a wrong byte on the console.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "detect/psram_info.h"
#include "hardware/dma.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/time.h"
#include "results/record.h"
#include "storage/flash_layout.h"

// 16 KB of "ROM": one Atari 2600 bank window is 4 KB, so this covers four banks.
#define ROM_BYTES   (16u * 1024u)
#define ROM_MASK    (ROM_BYTES - 1u)
#define BANK_BYTES  4096u
#define ADDR_COUNT  4096u             // synthetic address sequence, power of two
#define ADDR_MASK   (ADDR_COUNT - 1u)

// Free SRAM the test needs: the 16 KB table plus the 4 KB DMA ring placed at +32 KB.
// (RP2040 has ~112 KB free, so requiring 256 KB silently skipped the whole test.)
#define HOTPATH_RAM_NEEDED (48u * 1024u)

#define ITERATIONS  200000u
#define ITERATIONS_HEADLINE 1000000u

// Zero at run time; the compiler cannot know that, so the bus read stays in the path.
static volatile uint32_t s_bus_mask;

static uint32_t s_addr[ADDR_COUNT];
static uint32_t s_bank_base;

typedef enum { PAT_SEQUENTIAL, PAT_RANDOM, PAT_BANKSWITCH } pattern_t;

static void build_addresses(pattern_t pat) {
    uint32_t seed = 0x1A2B3C4Du;
    for (uint32_t i = 0; i < ADDR_COUNT; i++) {
        switch (pat) {
        case PAT_SEQUENTIAL:
            s_addr[i] = 0x1000u + (i & (BANK_BYTES - 1u));
            break;
        case PAT_RANDOM:
            s_addr[i] = 0x1000u + (bench_rand(&seed) & (BANK_BYTES - 1u));
            break;
        case PAT_BANKSWITCH:
            // Every eighth access lands on a bank-switch hotspot (0x1FF8..0x1FFB),
            // like an F8/F6 style cartridge.
            s_addr[i] = (i % 8u == 7u) ? 0x1FF8u + (bench_rand(&seed) & 3u)
                                       : 0x1000u + (bench_rand(&seed) & (BANK_BYTES - 1u));
            break;
        }
    }
}

/*
 * The hot path itself. Defined twice from one macro: once into SRAM and once left in
 * flash, so the cost of running the same code from XIP can be measured.
 *
 *   bus   = real GPIO input read
 *   addr  = synthetic address, made dependent on the bus read
 *   bank  = a write to the hotspot range switches bank, as a real mapper does
 *   data  = the byte the console would latch
 */
#define HOT_BODY(rom, rom_mask)                                             \
    uint32_t bus = sio_hw->gpio_in;                                         \
    uint32_t addr = next ^ (bus & s_bus_mask);                              \
    uint32_t off = addr & (BANK_BYTES - 1u);                                \
    if (addr >= 0x1FF8u) s_bank_base = (addr & 3u) * BANK_BYTES;            \
    uint8_t data = (rom)[(s_bank_base + off) & (rom_mask)];                 \
    sio_hw->gpio_out = (uint32_t)data << 8;

#define HOT_LOOP()                                                          \
    for (uint32_t i = 0; i < iterations; i++) {                             \
        uint32_t next = s_addr[i & ADDR_MASK];                              \
        uint32_t t0 = timing_now();                                         \
        HOT_BODY(rom, rom_mask)                                             \
        uint32_t t1 = timing_now();                                         \
        stats_stream_add(st, timing_net(t0, t1));                           \
    }

// The same loop in SRAM and in flash. Both must stay out-of-line, or the compiler
// would inline them into this flash-resident file and the comparison would be void.
static void __no_inline_not_in_flash_func(hot_from_sram)(const uint8_t *rom, uint32_t rom_mask,
                                                         uint32_t iterations, stats_stream_t *st) {
    HOT_LOOP()
}

static __attribute__((noinline)) void hot_from_flash(const uint8_t *rom, uint32_t rom_mask,
                                                     uint32_t iterations, stats_stream_t *st) {
    HOT_LOOP()
}

// ---- Background traffic ---------------------------------------------------------------

typedef enum { TRAFFIC_NONE, TRAFFIC_DMA_SRAM, TRAFFIC_DMA_PSRAM } traffic_t;

#define DMA_RING_BITS 12u          // addresses wrap inside an aligned 4 KB block
#define DMA_FOREVER   0x0FFFFFFFu  // fits RP2350's 28-bit count; far longer than a run

static int s_dma_ch = -1;
static uint32_t s_dma_words;
static volatile uint32_t s_dma_sink;

// Continuous traffic for the whole measurement, with the addresses bounded by a ring:
// restarting a finished transfer between chunks would leave the DMA idle for most of
// each chunk, and a self-chaining pair would walk out of its buffer.
static bool traffic_start(traffic_t t) {
    s_dma_words = 0;
    if (t == TRAFFIC_NONE) return true;
    s_dma_ch = dma_claim_unused_channel(false);
    if (s_dma_ch < 0) return false;
    uintptr_t src = t == TRAFFIC_DMA_SRAM ? (uintptr_t)(bench_ram() + 32u * 1024u)
                                          : PSRAM_NOCACHE_BASE;
    src = (src + (1u << DMA_RING_BITS) - 1u) & ~((uintptr_t)(1u << DMA_RING_BITS) - 1u);
    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_ring(&c, false, DMA_RING_BITS);
    dma_channel_configure(s_dma_ch, &c, (void *)&s_dma_sink, (const void *)src, DMA_FOREVER, true);
    return true;
}

static void traffic_stop(void) {
    if (s_dma_ch < 0) return;
    s_dma_words = DMA_FOREVER - dma_channel_hw_addr(s_dma_ch)->transfer_count;
    dma_channel_abort(s_dma_ch);
    dma_channel_unclaim(s_dma_ch);
    s_dma_ch = -1;
}

// A real periodic interrupt: the handler lives in flash, like a normal SDK callback,
// so its cost includes fetching it through XIP.
static repeating_timer_t s_timer;
static volatile uint32_t s_irq_count;

static bool irq_load_cb(repeating_timer_t *t) {
    (void)t;
    s_irq_count++;
    return true;
}

// ---- Runner -------------------------------------------------------------------------------

typedef struct {
    const char *name;
    const uint8_t *rom;
    uint32_t rom_mask;
    bool code_in_sram;
    pattern_t pattern;
    traffic_t traffic;
    bool irqs_enabled;
    bool cold_cache;      // invalidate the XIP cache every chunk
    uint32_t chunk;       // iterations between keepalives / cache invalidations
    uint32_t iterations;
} variant_t;

static void run_variant(test_ctx_t *ctx, const variant_t *v, bool with_histogram) {
    build_addresses(v->pattern);
    s_bank_base = 0;
    s_bus_mask = 0;
    stats_stream_t st;
    stats_stream_init(&st, g_bench_stream_bins, BENCH_STREAM_BINS);

    if (!traffic_start(v->traffic)) {
        rec_text(ctx, v->name, "no free DMA channel");
        return;
    }
    s_irq_count = 0;
    if (v->irqs_enabled) add_repeating_timer_us(-100, irq_load_cb, NULL, &s_timer);   // 10 kHz

    // Long runs are split into chunks: the watchdog needs feeding, and a cold-cache
    // variant needs the cache invalidated in between. The cold variant uses a short
    // chunk on purpose - with 20000 iterations between invalidations the table would
    // be back in the cache after the first few accesses and almost nothing would miss.
    const uint32_t chunk = v->chunk ? v->chunk : 20000u;
    uint32_t done = 0;
    while (done < v->iterations) {
        uint32_t n = v->iterations - done < chunk ? v->iterations - done : chunk;
        if (v->cold_cache) xip_cache_invalidate_all();
        uint32_t irq = v->irqs_enabled ? 0 : save_and_disable_interrupts();
        if (v->code_in_sram) hot_from_sram(v->rom, v->rom_mask, n, &st);
        else hot_from_flash(v->rom, v->rom_mask, n, &st);
        if (!v->irqs_enabled) restore_interrupts(irq);
        guard_keepalive();
        done += n;
    }
    if (v->irqs_enabled) cancel_repeating_timer(&s_timer);
    traffic_stop();

    stats_t s;
    stats_stream_finish(&st, &s);
    rec_stats(ctx, v->name, UNIT_CYCLES, 1, &s);
    char key[REC_LABEL_LEN + 24];
    if (s_dma_words) {
        snprintf(key, sizeof(key), "%.*s_dma_words", (int)(sizeof(key) - 12), v->name);
        rec_value(ctx, key, UNIT_COUNT, s_dma_words);
    }
    if (v->irqs_enabled) {
        snprintf(key, sizeof(key), "%.*s_irqs", (int)(sizeof(key) - 6), v->name);
        rec_value(ctx, key, UNIT_COUNT, s_irq_count);
    }
    if (with_histogram) {
        // Exact per-cycle histogram of the fast part of the distribution.
        char label[REC_LABEL_LEN + 24];
        snprintf(label, sizeof(label), "%.*s_hist", (int)(sizeof(label) - 6), v->name);
        rec_hist(ctx, label, HIST_LINEAR, 0, 1, 128, g_bench_stream_bins);
    }
}

// ---- The tests ------------------------------------------------------------------------------

// A ROM image in SRAM, and pointers to the same content in flash and PSRAM.
static uint8_t *sram_rom(void) {
    uint8_t *rom = bench_ram();
    for (uint32_t i = 0; i < ROM_BYTES; i++) rom[i] = (uint8_t)(i * 7u + 3u);
    return rom;
}

test_result_t test_hotpath_memory(test_ctx_t *ctx) {
    if (bench_ram_size() < HOTPATH_RAM_NEEDED) return TR_NA;
    const uint8_t *rom_sram = sram_rom();
    const uint8_t *rom_flash = (const uint8_t *)XIP_BASE;               // the firmware image
    const uint8_t *rom_flash_uncached = (const uint8_t *)XIP_NOCACHE_NOALLOC_BASE;

    rec_text(ctx, "shape", "bus read -> decode -> table fetch -> data write, one timed iteration "
                           "per console bus cycle; %lu-byte table, 4 KB banks", (unsigned long)ROM_BYTES);

    variant_t v = {.rom = rom_sram, .rom_mask = ROM_MASK, .code_in_sram = true,
                   .pattern = PAT_RANDOM, .iterations = ITERATIONS_HEADLINE};

    v.name = "sram_table_sram_code";
    run_variant(ctx, &v, true);

    v.name = "sram_table_flash_code";
    v.code_in_sram = false;
    v.iterations = ITERATIONS;
    run_variant(ctx, &v, false);

    v.name = "flash_table_warm";
    v.rom = rom_flash;
    v.code_in_sram = true;
    run_variant(ctx, &v, true);

    // Cache invalidated every 256 accesses: models a ROM far larger than the 16 KB
    // cache, where a random access is nearly always a miss.
    v.name = "flash_table_cold_cache";
    v.cold_cache = true;
    v.chunk = 256u;
    v.iterations = 100000u;
    run_variant(ctx, &v, true);
    v.cold_cache = false;
    v.chunk = 0;
    v.iterations = ITERATIONS;

    v.name = "flash_table_uncached";
    v.rom = rom_flash_uncached;
    run_variant(ctx, &v, false);

#if !PICO_RP2040
    if (g_psram.present && g_psram.initialised) {
        uint8_t *ps = (uint8_t *)PSRAM_NOCACHE_BASE;
        for (uint32_t i = 0; i < ROM_BYTES; i++) ps[i] = (uint8_t)(i * 7u + 3u);
        v.name = "psram_table_uncached";
        v.rom = ps;
        run_variant(ctx, &v, false);

        v.name = "psram_table_cached";
        v.rom = (const uint8_t *)PSRAM_CACHED_BASE;
        run_variant(ctx, &v, true);
    }
#endif
    return TR_PASS;
}

test_result_t test_hotpath_disturbance(test_ctx_t *ctx) {
    if (bench_ram_size() < HOTPATH_RAM_NEEDED) return TR_NA;
    const uint8_t *rom_sram = sram_rom();
    const uint8_t *rom_flash = (const uint8_t *)XIP_BASE;

    variant_t v = {.rom = rom_sram, .rom_mask = ROM_MASK, .code_in_sram = true,
                   .pattern = PAT_RANDOM, .iterations = ITERATIONS};

    v.name = "sram_quiet";
    run_variant(ctx, &v, false);

    v.name = "sram_irqs_10khz";
    v.irqs_enabled = true;
    run_variant(ctx, &v, false);
    v.irqs_enabled = false;

    v.name = "sram_dma_sram";
    v.traffic = TRAFFIC_DMA_SRAM;
    run_variant(ctx, &v, false);

    v.name = "flash_table_quiet";
    v.rom = rom_flash;
    v.traffic = TRAFFIC_NONE;
    run_variant(ctx, &v, false);

    v.name = "flash_table_dma_sram";
    v.traffic = TRAFFIC_DMA_SRAM;
    run_variant(ctx, &v, false);

#if !PICO_RP2040
    if (g_psram.present && g_psram.initialised) {
        // The worst case found in phase 6: flash lookups while PSRAM keeps QMI busy.
        v.name = "flash_table_dma_psram";
        v.traffic = TRAFFIC_DMA_PSRAM;
        run_variant(ctx, &v, true);

        v.name = "sram_table_dma_psram";
        v.rom = rom_sram;
        run_variant(ctx, &v, true);
    }
#endif
    rec_text(ctx, "note", "the same hot path under load: interrupts enabled, DMA in SRAM, and DMA "
                          "on PSRAM, which shares the QMI controller with flash");
    return TR_PASS;
}

test_result_t test_hotpath_patterns(test_ctx_t *ctx) {
    if (bench_ram_size() < HOTPATH_RAM_NEEDED) return TR_NA;
    const uint8_t *rom_sram = sram_rom();
    variant_t v = {.rom = rom_sram, .rom_mask = ROM_MASK, .code_in_sram = true,
                   .iterations = ITERATIONS};

    v.name = "pattern_sequential";
    v.pattern = PAT_SEQUENTIAL;
    run_variant(ctx, &v, false);

    v.name = "pattern_random";
    v.pattern = PAT_RANDOM;
    run_variant(ctx, &v, false);

    v.name = "pattern_bankswitch";
    v.pattern = PAT_BANKSWITCH;
    run_variant(ctx, &v, true);

    rec_text(ctx, "note", "bankswitch: every eighth access lands on a hotspot at 0x1FF8-0x1FFB and "
                          "changes the active bank, as an F8/F6 style mapper does");
    return TR_PASS;
}
