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
