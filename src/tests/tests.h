// Test entry points, grouped by source file. Registered in src/tests/registry.c.
#ifndef PICOBENCH_TESTS_H
#define PICOBENCH_TESTS_H

#include "core/sequencer.h"

// detect/inventory.c, detect/flash_info.c, detect/psram_detect.c - hardware inventory
test_result_t test_inv_chip(test_ctx_t *ctx);
test_result_t test_inv_firmware(test_ctx_t *ctx);
test_result_t test_inv_ids(test_ctx_t *ctx);
test_result_t test_inv_board(test_ctx_t *ctx);
test_result_t test_inv_memory(test_ctx_t *ctx);
test_result_t test_inv_reset(test_ctx_t *ctx);
test_result_t test_inv_environment(test_ctx_t *ctx);
test_result_t test_inv_clock_sources(test_ctx_t *ctx);
test_result_t test_inv_usb(test_ctx_t *ctx);
test_result_t test_flash_identify(test_ctx_t *ctx);
test_result_t test_flash_interface(test_ctx_t *ctx);
test_result_t test_psram_detect(test_ctx_t *ctx);

// cpu.c - CPU benchmarks
test_result_t test_cpu_integer(test_ctx_t *ctx);
test_result_t test_cpu_branch(test_ctx_t *ctx);
test_result_t test_cpu_memory_ops(test_ctx_t *ctx);
test_result_t test_cpu_crc32(test_ctx_t *ctx);
test_result_t test_cpu_lut(test_ctx_t *ctx);

// sram_integrity.c, sram_bench.c - SRAM correctness and performance
test_result_t test_sram_integrity(test_ctx_t *ctx);
test_result_t test_sram_throughput(test_ctx_t *ctx);
test_result_t test_sram_stride(test_ctx_t *ctx);
test_result_t test_sram_latency(test_ctx_t *ctx);
test_result_t test_sram_contention_dma(test_ctx_t *ctx);

// flash_xip.c, flash_write.c, flash_mode.c - flash, XIP and cache
test_result_t test_flash_read(test_ctx_t *ctx);
test_result_t test_flash_cache(test_ctx_t *ctx);
test_result_t test_flash_code_exec(test_ctx_t *ctx);
test_result_t test_flash_copy_to_sram(test_ctx_t *ctx);
test_result_t test_flash_write_scratch(test_ctx_t *ctx);
test_result_t test_flash_xip_mode_sweep(test_ctx_t *ctx);

// psram_integrity.c, psram_bench.c, qmi_conflict.c - PSRAM (RP2350 with a chip fitted)
test_result_t test_psram_integrity(test_ctx_t *ctx);
test_result_t test_psram_bench(test_ctx_t *ctx);
test_result_t test_psram_copy(test_ctx_t *ctx);
test_result_t test_psram_timing_sweep(test_ctx_t *ctx);
test_result_t test_qmi_conflict(test_ctx_t *ctx);

// hotpath.c - the cartridge hot path
test_result_t test_hotpath_memory(test_ctx_t *ctx);
test_result_t test_hotpath_disturbance(test_ctx_t *ctx);
test_result_t test_hotpath_patterns(test_ctx_t *ctx);

// gpio_sio.c, pio.c, dma.c, multicore.c, irq_timer.c - phase 8
test_result_t test_gpio_sio(test_ctx_t *ctx);
test_result_t test_pio_inventory(test_ctx_t *ctx);
test_result_t test_pio_fifo(test_ctx_t *ctx);
test_result_t test_pio_irq(test_ctx_t *ctx);
test_result_t test_pio_dma(test_ctx_t *ctx);
test_result_t test_dma_channels(test_ctx_t *ctx);
test_result_t test_dma_transfer(test_ctx_t *ctx);
test_result_t test_dma_parallel(test_ctx_t *ctx);
test_result_t test_dma_chain_and_pacing(test_ctx_t *ctx);
test_result_t test_multicore(test_ctx_t *ctx);
test_result_t test_irq_latency(test_ctx_t *ctx);
test_result_t test_irq_storm(test_ctx_t *ctx);

// selftest.c - infrastructure self-checks
test_result_t test_timing_counter(test_ctx_t *ctx);
test_result_t test_clocks_readback(test_ctx_t *ctx);
test_result_t test_cpu_loop_smoke(test_ctx_t *ctx);
test_result_t test_debug_crash(test_ctx_t *ctx);

#endif
