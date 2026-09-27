/*
 * The test table. Order here is execution order at the default operating point.
 *
 * IDs are stable: never renumber or reuse an ID, reports from different firmware
 * versions are compared by ID. Ranges:
 *   1xx core/clocks, 2xx CPU, 3xx SRAM, 4xx flash/XIP, 5xx PSRAM, 6xx DMA, 7xx PIO/GPIO,
 *   8xx multicore/IRQ, 9xx misc (crypto, ADC, USB), 99x debug-only.
 */

#include "tests/tests.h"

#define INV TF_INVENTORY

const test_desc_t g_tests[] = {
    // ---- Inventory (HARDWARE.TXT) -------------------------------------------------------
    {110, SUBSYS_CORE, CHIP_ALL, INV, "core.chip", "Chip identification", test_inv_chip},
    {111, SUBSYS_CORE, CHIP_ALL, INV, "core.firmware", "Firmware, SDK, compiler and boot ROM", test_inv_firmware},
    {112, SUBSYS_CORE, CHIP_ALL, INV, "core.ids", "Unique identifiers", test_inv_ids},
    {113, SUBSYS_CORE, CHIP_ALL, INV, "core.board", "Board profile (inferred)", test_inv_board},
    {114, SUBSYS_CORE, CHIP_ALL, INV, "core.memory", "Memory map and flash layout", test_inv_memory},
    {115, SUBSYS_CORE, CHIP_ALL, INV, "core.reset", "Reset reason", test_inv_reset},
    {116, SUBSYS_CORE, CHIP_ALL, INV, "core.environment", "Temperature and supply", test_inv_environment},
    {102, SUBSYS_CLOCKS, CHIP_ALL, INV, "clocks.sources", "Clock sources, PLLs and operating points",
     test_inv_clock_sources},
    {400, SUBSYS_FLASH, CHIP_ALL, INV, "flash.identify", "Flash chip: JEDEC, SFDP, status, unique ID",
     test_flash_identify},
    {401, SUBSYS_FLASH, CHIP_ALL, INV, "flash.interface", "Flash interface (SSI / QMI) configuration",
     test_flash_interface},
    {500, SUBSYS_PSRAM, CHIP_RP2350, INV | TF_RISKY, "psram.detect",
     "PSRAM auto-detection on every XIP_CS1 GPIO, init and sanity check", test_psram_detect},
    // After psram.detect and core.environment: uses what they found.
    {117, SUBSYS_GPIO, CHIP_ALL, INV, "core.pins",
     "Pins taken on this board, and pins free for your own hardware", test_inv_pins},
    {910, SUBSYS_USB, CHIP_ALL, INV, "usb.config", "USB device configuration", test_inv_usb},

    // ---- Self-checks ----------------------------------------------------------------------
    {100, SUBSYS_CORE, CHIP_ALL, 0, "core.timing_counter",
     "Cycle counter read cost and cross-check against the 1 MHz timer",
     test_timing_counter},
    {101, SUBSYS_CLOCKS, CHIP_ALL, TF_SWEEP, "clocks.readback",
     "Measured clk_sys and PLL output versus the requested operating point",
     test_clocks_readback},
    {200, SUBSYS_CPU, CHIP_ALL, TF_SWEEP, "cpu.loop_smoke",
     "Empty 100-iteration loop from SRAM, IRQs off (scaling smoke test)",
     test_cpu_loop_smoke},

    // ---- CPU ------------------------------------------------------------------------------
    {201, SUBSYS_CPU, CHIP_ALL, TF_SWEEP, "cpu.integer",
     "Cost of add/and/shift/mul/udiv/sdiv per operation, from SRAM", test_cpu_integer},
    {202, SUBSYS_CPU, CHIP_ALL, 0, "cpu.branch",
     "Predictable versus data-dependent branches", test_cpu_branch},
    {203, SUBSYS_CPU, CHIP_ALL, 0, "cpu.memory_ops",
     "memcpy / memset / 32-bit copy loop in SRAM, 64 B to 16 KB", test_cpu_memory_ops},
    {204, SUBSYS_CPU, CHIP_ALL, 0, "cpu.crc32_sw",
     "Software CRC-32 over 4 KB of SRAM", test_cpu_crc32},
    {205, SUBSYS_CPU, CHIP_ALL, 0, "cpu.lut",
     "Random and sequential lookups in 4/16/64 KB SRAM tables", test_cpu_lut},

    // ---- SRAM -----------------------------------------------------------------------------
    {300, SUBSYS_SRAM, CHIP_ALL, 0, "sram.integrity",
     "Patterns, address tests, byte/halfword access and March C- over all free SRAM",
     test_sram_integrity},
    {301, SUBSYS_SRAM, CHIP_ALL, TF_SWEEP, "sram.throughput",
     "8/16/32-bit sequential read and write per bank group", test_sram_throughput},
    {302, SUBSYS_SRAM, CHIP_ALL, 0, "sram.stride",
     "Read cost versus access stride (bank conflicts)", test_sram_stride},
    {303, SUBSYS_SRAM, CHIP_ALL, TF_SWEEP, "sram.latency",
     "Dependent-load latency, random order, 4 KB to 256 KB", test_sram_latency},
    // ---- Flash / XIP / cache ----------------------------------------------------------
    {410, SUBSYS_FLASH, CHIP_ALL, TF_SWEEP, "flash.read",
     "XIP read throughput: cached warm, streaming, and through the uncached alias", test_flash_read},
    {411, SUBSYS_FLASH, CHIP_ALL, 0, "flash.cache",
     "Cache hit and cold-miss latency, per-line misses, working-set sweep, invalidate cost",
     test_flash_cache},
    {412, SUBSYS_FLASH, CHIP_ALL, TF_SWEEP, "flash.code_exec",
     "The same loop executed from SRAM, from flash warm, and from flash with a cold cache",
     test_flash_code_exec},
    {413, SUBSYS_FLASH, CHIP_ALL, 0, "flash.copy_to_sram",
     "memcpy from flash to SRAM, 1 KB to 64 KB, cached and uncached", test_flash_copy_to_sram},
    {420, SUBSYS_FLASH, CHIP_ALL, TF_RISKY, "flash.write_scratch",
     "Erase/program/verify timings in the reserved 64 KB scratch area", test_flash_write_scratch},
    {430, SUBSYS_FLASH, CHIP_RP2350, TF_RISKY, "flash.xip_mode_sweep",
     "Read mode (serial/dual/quad) and clock divider sweep, verified by checksum and restored",
     test_flash_xip_mode_sweep},

    // ---- PSRAM ----------------------------------------------------------------------------
    {510, SUBSYS_PSRAM, CHIP_RP2350, TF_RISKY, "psram.integrity",
     "Patterns, address and size checks, narrow accesses and March C- over the PSRAM",
     test_psram_integrity},
    {511, SUBSYS_PSRAM, CHIP_RP2350, TF_SWEEP, "psram.bench",
     "PSRAM read/write throughput and latency, cached and uncached", test_psram_bench},
    {512, SUBSYS_PSRAM, CHIP_RP2350, 0, "psram.copy",
     "Copies SRAM<->PSRAM and PSRAM->PSRAM, by CPU and by DMA", test_psram_copy},
    {513, SUBSYS_PSRAM, CHIP_RP2350, TF_RISKY, "psram.timing_sweep",
     "QMI divisor and RX delay sweep, each verified by a write/read stress test",
     test_psram_timing_sweep},
    {520, SUBSYS_PSRAM, CHIP_RP2350, 0, "qmi.conflict",
     "What PSRAM traffic costs flash reads, and the other way round", test_qmi_conflict},

    // ---- Cartridge hot path -----------------------------------------------------------
    {600, SUBSYS_CPU, CHIP_ALL, TF_SWEEP, "hotpath.memory",
     "Address decode and byte fetch with the table in SRAM, flash (warm/cold/uncached) and PSRAM",
     test_hotpath_memory},
    {601, SUBSYS_CPU, CHIP_ALL, 0, "hotpath.disturbance",
     "The same path with interrupts, SRAM DMA and PSRAM DMA running", test_hotpath_disturbance},
    {602, SUBSYS_CPU, CHIP_ALL, 0, "hotpath.patterns",
     "Sequential, random and bank-switching address patterns", test_hotpath_patterns},

    // ---- PIO, GPIO, DMA, multicore, interrupts ------------------------------------------
    {700, SUBSYS_PIO, CHIP_ALL, 0, "pio.inventory",
     "Every PIO block and state machine loads a program and echoes a word", test_pio_inventory},
    {701, SUBSYS_PIO, CHIP_ALL, TF_SWEEP, "pio.fifo",
     "CPU-PIO-CPU round trip, FIFO write cost and sustained rate", test_pio_fifo},
    {702, SUBSYS_PIO, CHIP_ALL, 0, "pio.irq",
     "Time from a trigger word to the PIO interrupt flag being visible", test_pio_irq},
    {703, SUBSYS_PIO, CHIP_ALL, 0, "pio.dma",
     "DMA feeding a state machine at full speed", test_pio_dma},
    {710, SUBSYS_GPIO, CHIP_ALL, 0, "gpio.sio",
     "SIO register costs and output-to-input turnaround (internal timing only)", test_gpio_sio},
    {720, SUBSYS_DMA, CHIP_ALL, 0, "dma.channels",
     "Every DMA channel copies and verifies a block", test_dma_channels},
    {721, SUBSYS_DMA, CHIP_ALL, TF_SWEEP, "dma.transfer",
     "DMA transfer cost and rate, 64 B to 64 KB, plus single-word overhead", test_dma_transfer},
    {722, SUBSYS_DMA, CHIP_ALL, 0, "dma.parallel",
     "1, 2, 4 and 8 channels copying at once: where the bus saturates", test_dma_parallel},
    {723, SUBSYS_DMA, CHIP_ALL, 0, "dma.chain_pacing",
     "Chained transfers and timer-paced transfers", test_dma_chain_and_pacing},
    {730, SUBSYS_MULTICORE, CHIP_ALL, 0, "multicore.basic",
     "Core 1 launch, inter-core FIFO round trip, spinlock cost and core-to-core contention",
     test_multicore},
    {740, SUBSYS_IRQ, CHIP_ALL, TF_SWEEP, "irq.latency",
     "Software interrupt entry and round trip, alarm latency, and the same with DMA running",
     test_irq_latency},
    {741, SUBSYS_IRQ, CHIP_ALL, 0, "irq.storm",
     "Foreground work under a 50 kHz interrupt storm", test_irq_storm},

    {310, SUBSYS_SRAM, CHIP_ALL, 0, "sram.contention_dma",
     "CPU read loop with a DMA stream in the same and in the other bank group",
     test_sram_contention_dma},
#ifdef PICOBENCH_TEST_CRASH
    {990, SUBSYS_CORE, CHIP_ALL, TF_RISKY, "debug.crash",
     "Hangs on purpose to exercise watchdog recovery (debug builds only)",
     test_debug_crash},
#endif
};

const unsigned g_test_count = sizeof(g_tests) / sizeof(g_tests[0]);
