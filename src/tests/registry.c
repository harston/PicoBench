/*
 * The test table. Order here is execution order at the default operating point.
 *
 * IDs are stable: never renumber or reuse an ID, reports from different firmware
 * versions are compared by ID. Ranges:
 *   1xx core/clocks, 2xx CPU, 3xx SRAM, 4xx flash/XIP, 5xx PSRAM, 6xx DMA, 7xx PIO/GPIO,
 *   8xx multicore/IRQ, 9xx misc (crypto, ADC, USB), 99x debug-only.
 */

#include "tests/tests.h"

const test_desc_t g_tests[] = {
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
