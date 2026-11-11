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
#include "pico/multicore.h"
#include "results/crc32.h"
#include "tests/arm/awl.h"
#include "tests/arm/synth_load.h"

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

#if PICO_RP2040
// RP2040: the address list lives in the sample buffer, which the hot path tests do not
// use otherwise (statistics go through g_bench_stream_bins; bench_group_block() never
// returns it on a one-group chip). Its 16 KB went to the record arena, which the 150 and
// 200 MHz operating points had filled to 94 % (results_9). Both are 32-byte aligned, so
// the list keeps its bank phase.
#define s_addr g_bench_samples
_Static_assert(BENCH_SAMPLES_MAX >= ADDR_COUNT, "sample buffer too small for the address list");
#else
static uint32_t s_addr[ADDR_COUNT];
#endif
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

// ---- ARM interpreter on the other core -------------------------------------------------
//
// The cartridge design this is for (PicoA10400 CHAMP_ARM_TODO.md, stages 2-4): one
// core serves the console bus, the other runs a game's ARM code through an
// interpreter or translated code - and both work out of SRAM. The question is
// what the second core's memory traffic does to the first core's response time.
//
// Roles are swapped here for convenience (core 0 measures the hot path, core 1
// interprets); the cores are symmetric masters on the bus fabric, so the effect
// is the same. Core 1 runs one ARM program over and over, restoring its RAM from a
// copy first, entirely from SRAM:
//   - always: the synthetic load (src/tests/arm/synth_load.h) - written for PicoBench,
//     no game code, so this test runs in every build, the published ones included;
//   - with a local workload (workload/README.md) also the heaviest event-free call of
//     Galagon (or of the workload's first title), as a check that the synthetic load
//     disturbs the hot path the way real game code does.
//
// Where the memory is: the hot path table sits at bench_ram(), i.e. in the bank group
// holding the free heap - group 0 on the RP2040, group 1 on the RP2350, whose firmware
// data fills group 0. "same" puts the interpreter's buffers right after the table;
// "other" (RP2350) puts them in the other group, which there can only be the tail of
// the sample buffer (bench_group_block(); this test does not use g_bench_samples).
// Until 2026-09-29 the test demanded 64 KB of heap in group 0 and so reported N/A on
// every RP2350. What stays fixed wherever the buffers go: the interpreter's dispatch
// table (tbl.0 in the .dis; group 0 on every chip) and the hot path's address list and
// histogram are statics at link-time addresses. The recorded *_addr values say where
// the rest was.

#define NB_TITLE "galagon"
#define NB_BYTES(rom, ram) ((((rom) + 3u) & ~3u) + 2u * (ram))   // ROM image, RAM, RAM copy

static volatile uint32_t s_nb_run, s_nb_ready, s_nb_loops;
static thumb_cpu_t s_nb_cpu;          // the staged call, copied before every run
static uint32_t *s_nb_ram, *s_nb_ram_init;
static uint32_t s_nb_ram_words;

static void __no_inline_not_in_flash_func(nb_core1)(void) {
    s_nb_ready = 1;
    while (s_nb_run) {
        // Word copies by hand, with a compiler fence in each loop: without it GCC
        // recognises the loops and emits memcpy, which is in flash (seen in the
        // first build: a call to ____wrap_memcpy_veneer from this function).
        for (uint32_t i = 0; i < s_nb_ram_words; i++) {
            s_nb_ram[i] = s_nb_ram_init[i];
            __asm volatile("" ::: "memory");
        }
        thumb_cpu_t cpu;
        const uint32_t *src = (const uint32_t *)&s_nb_cpu;
        uint32_t *dst = (uint32_t *)&cpu;
        for (uint32_t i = 0; i < sizeof(thumb_cpu_t) / 4u; i++) {
            dst[i] = src[i];
            __asm volatile("" ::: "memory");
        }
        thumb_run(&cpu, 10u * 1000u * 1000u);
        s_nb_loops++;
    }
    s_nb_ready = 0;
}

static bool nb_start(void) {
    s_nb_loops = 0;
    s_nb_run = 1;
    s_nb_ready = 0;
    multicore_launch_core1(nb_core1);
    uint32_t spins = 0;
    while (!s_nb_ready && ++spins < 100000000u) {}
    return s_nb_ready;
}

static void nb_stop(void) {
    s_nb_run = 0;
    uint32_t spins = 0;
    while (s_nb_ready && ++spins < 100000000u) {}
    multicore_reset_core1();
}

// Keep the staged state (cpu in *cpu, RAM in ram) for core 1: the RAM copy goes to init.
static void nb_keep(const thumb_cpu_t *cpu, uint8_t *ram, uint8_t *init, uint32_t ram_size) {
    memcpy(init, ram, ram_size);
    s_nb_cpu = *cpu;
    s_nb_ram = (uint32_t *)ram;
    s_nb_ram_init = (uint32_t *)init;
    s_nb_ram_words = ram_size / 4u;
}

// The synthetic load at `base`. One run is made here on core 0 and checked against
// the expected result before core 1 gets it. Returns its instruction count, 0 if wrong.
static uint32_t nb_stage_synth(uint8_t *base) {
    uint8_t *rom = base, *ram = base + SYNTH_ROM_SIZE, *init = ram + SYNTH_RAM_SIZE;
    thumb_cpu_t cpu, run;
    synth_load_stage(rom, ram, &cpu);
    memcpy(init, ram, SYNTH_RAM_SIZE);
    run = cpu;
    if (thumb_run(&run, 10u * 1000u * 1000u) != THUMB_ENDED || run.count != SYNTH_INSTRUCTIONS ||
        crc32_update(0, ram, SYNTH_RAM_SIZE) != SYNTH_RAM_CRC)
        return 0;
    memcpy(ram, init, SYNTH_RAM_SIZE);
    nb_keep(&cpu, ram, init, SYNTH_RAM_SIZE);
    return SYNTH_INSTRUCTIONS;
}

// The heaviest event-free recorded call of title t at `base`. Returns its instruction
// count, 0 on failure.
static uint32_t nb_stage_game(uint8_t *base, const awl_title_t *t) {
    uint32_t rom_bytes = (t->rom_size + 3u) & ~3u;
    uint8_t *rom = base, *ram = base + rom_bytes, *init = ram + t->ram_size;
    awl_load_rom(t, rom);
    uint32_t best = 0, best_k = 0, instr, events;
    thumb_cpu_t cpu;
    for (unsigned k = 0; k < t->ncalls; k++) {
        if (!awl_prepare_call(t, rom, ram, k, &cpu, &instr, &events)) return 0;
        if (events == 0 && instr > best) { best = instr; best_k = k; }
    }
    if (!best || !awl_prepare_call(t, rom, ram, best_k, &cpu, &instr, &events)) return 0;
    nb_keep(&cpu, ram, init, t->ram_size);
    return best;
}

// Core 1's interpreter speed (guest MIPS x10) over `us` microseconds.
static void nb_record_core1(test_ctx_t *ctx, const char *label, uint32_t loops, uint32_t instr, uint32_t us) {
    char k[REC_LABEL_LEN + 8];
    snprintf(k, sizeof k, "%.14s_c1_mips_x10", label);
    rec_value(ctx, k, UNIT_COUNT, us ? (int64_t)((uint64_t)loops * instr * 10u / us) : 0);
}

// One variant: core 1 interprets the staged program while core 0 runs the hot path.
static bool nb_variant(test_ctx_t *ctx, variant_t *v, const char *name, const char *c1_label,
                       uint32_t instr) {
    if (!nb_start()) {
        rec_error(ctx, "core1_launch", 0, 1, 0);
        return false;
    }
    v->name = name;
    uint32_t t0 = time_us_32(), l0 = s_nb_loops;
    run_variant(ctx, v, true);
    nb_record_core1(ctx, c1_label, s_nb_loops - l0, instr, time_us_32() - t0);
    nb_stop();
    guard_keepalive();
    return true;
}

test_result_t test_hotpath_arm_neighbour(test_ctx_t *ctx) {
    const unsigned home = bench_group_of(bench_ram());
    uint32_t home_size;
    bench_group_ram(home, &home_size);
    const uint32_t synth_bytes = NB_BYTES(SYNTH_ROM_SIZE, SYNTH_RAM_SIZE);
    if (home_size < ROM_BYTES + synth_bytes) {
        rec_text(ctx, "sram", "needs %lu KB of free heap in one bank group, %lu KB free",
                 (unsigned long)((ROM_BYTES + synth_bytes) / 1024u), (unsigned long)(home_size / 1024u));
        return TR_NA;
    }
    rec_text(ctx, "setup", "core 0: hot path (SRAM table, random pattern, IRQs off); core 1: an ARM "
                           "program in the Thumb interpreter, looped from SRAM");

    const uint8_t *rom_sram = sram_rom();           // the table, at bench_ram()
    uint8_t *same = bench_ram() + ROM_BYTES;        // right after it, same group
    variant_t v = {.rom = rom_sram, .rom_mask = ROM_MASK, .code_in_sram = true,
                   .pattern = PAT_RANDOM, .iterations = ITERATIONS};
    rec_value(ctx, "table_addr", UNIT_HEX, (int64_t)(uintptr_t)rom_sram);
    rec_value(ctx, "hot_addr_list_addr", UNIT_HEX, (int64_t)(uintptr_t)s_addr);

    // ---- baseline: core 1 held in reset ----
    multicore_reset_core1();
    v.name = "neighbour_none";
    run_variant(ctx, &v, true);

    // ---- synthetic load, buffers in the same group as the table ----
    uint32_t instr = nb_stage_synth(same);
    if (!instr) {
        rec_error(ctx, "synth_result", 0, SYNTH_INSTRUCTIONS, 0);
        return TR_FAIL;
    }
    rec_value(ctx, "synth_instructions", UNIT_COUNT, instr);
    rec_value(ctx, "synth_same_addr", UNIT_HEX, (int64_t)(uintptr_t)same);
    // core 1 alone for a moment: its speed without the hot path competing
    if (!nb_start()) {
        rec_error(ctx, "core1_launch", 0, 1, 0);
        return TR_FAIL;
    }
    uint32_t t0 = time_us_32(), l0 = s_nb_loops;
    while (time_us_32() - t0 < 200000u) {}
    nb_record_core1(ctx, "core0_idle", s_nb_loops - l0, instr, time_us_32() - t0);
    nb_stop();
    if (!nb_variant(ctx, &v, "neighbour_synth_same", "synth_same", instr)) return TR_FAIL;

    // ---- synthetic load, buffers in the other group (RP2350) ----
    const char *source = NULL;
    uint8_t *other = bench_group_block(1u - home, synth_bytes, 0, &source);
    if (other) {
        if (!nb_stage_synth(other)) {
            rec_error(ctx, "synth_result_other", 0, SYNTH_INSTRUCTIONS, 0);
            return TR_FAIL;
        }
        rec_value(ctx, "synth_other_addr", UNIT_HEX, (int64_t)(uintptr_t)other);
        rec_text(ctx, "synth_other_source", "%s", source);
        if (!nb_variant(ctx, &v, "neighbour_synth_other", "synth_other", instr)) return TR_FAIL;
    } else {
        rec_text(ctx, "other_group", BENCH_BANK_GROUPS > 1 ? "no free block in the other bank group"
                                                            : "this chip has only one large bank group");
    }

    // ---- recorded game code (only with a local workload), same group ----
    awl_title_t t;
    bool found = false;
    for (unsigned i = 0; i < awl_ntitles() && !found; i++)
        found = awl_title(i, &t) && strcmp(t.name, NB_TITLE) == 0;
    if (!found && awl_ntitles() > 0) found = awl_title(0, &t);
    if (!found) {
        rec_text(ctx, "game", "no workload in this build - it holds game code, so it is generated "
                              "locally from your own ROMs (workload/README.md); the synthetic load stands in");
    } else if (home_size < ROM_BYTES + NB_BYTES(t.rom_size, t.ram_size)) {
        rec_text(ctx, "game", "%s needs %lu KB after the table, %lu KB free", t.name,
                 (unsigned long)(NB_BYTES(t.rom_size, t.ram_size) / 1024u),
                 (unsigned long)((home_size - ROM_BYTES) / 1024u));
    } else {
        instr = nb_stage_game(same, &t);
        if (!instr) {
            rec_error(ctx, "stage_call", 0, 1, 0);
            return TR_FAIL;
        }
        rec_text(ctx, "game", "%s (%s), heaviest event-free call, workload crc32 %08lx", t.name, t.format,
                 (unsigned long)awl_id());
        rec_value(ctx, "game_instructions", UNIT_COUNT, instr);
        if (!nb_variant(ctx, &v, "neighbour_game_same", "game_same", instr)) return TR_FAIL;
    }

    rec_text(ctx, "core1_state", "stopped and reset; later tests run single-core again");
    rec_text(ctx, "note", "compare p99.9 and max of neighbour_* with neighbour_none: that is what a "
                          "busy ARM executor on the other core costs the bus response");
    return TR_PASS;
}
