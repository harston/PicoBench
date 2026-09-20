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
#ifdef PICOBENCH_TEST_CRASH
    {990, SUBSYS_CORE, CHIP_ALL, TF_RISKY, "debug.crash",
     "Hangs on purpose to exercise watchdog recovery (debug builds only)",
     test_debug_crash},
#endif
};

const unsigned g_test_count = sizeof(g_tests) / sizeof(g_tests[0]);
