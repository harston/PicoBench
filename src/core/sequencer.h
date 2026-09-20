/*
 * Test registry and sequencer.
 *
 * Tests are listed in a static table (src/tests/registry.c). The sequencer expands
 * the table into a deterministic list of stages:
 *
 *   stages 0..T-1        : every test at the default operating point
 *   then, for each other operating point: every test flagged TF_SWEEP
 *
 * A stage number therefore identifies (operating point, test) across resets, which is
 * what lets a run resume after a watchdog reset and skip the stage that crashed.
 */

#ifndef PICOBENCH_SEQUENCER_H
#define PICOBENCH_SEQUENCER_H

#include <stdint.h>
#include <stdbool.h>

#include "results/record.h"

typedef enum {
    SUBSYS_CORE = 0,
    SUBSYS_CLOCKS,
    SUBSYS_CPU,
    SUBSYS_SRAM,
    SUBSYS_FLASH,
    SUBSYS_PSRAM,
    SUBSYS_DMA,
    SUBSYS_PIO,
    SUBSYS_GPIO,
    SUBSYS_MULTICORE,
    SUBSYS_IRQ,
    SUBSYS_USB,
    SUBSYS_CRYPTO,
    SUBSYS_ADC,
    SUBSYS_COUNT
} subsys_t;

// Chip variants a test applies to (runtime-detected).
#define CHIP_RP2040   0x1u
#define CHIP_RP2350A  0x2u
#define CHIP_RP2350B  0x4u
#define CHIP_RP2350   (CHIP_RP2350A | CHIP_RP2350B)
#define CHIP_ALL      (CHIP_RP2040 | CHIP_RP2350)

// Test flags.
#define TF_SWEEP  0x1u   // also run at every non-default operating point
#define TF_RISKY  0x2u   // may hang or reset (changes QMI timing, writes flash, ...)
#define TF_INVENTORY 0x4u // identifies hardware/conditions; rendered into HARDWARE.TXT

typedef struct test_ctx test_ctx_t;

typedef struct {
    uint16_t id;             // stable ID, used in reports to compare runs
    uint8_t subsys;          // subsys_t
    uint8_t chip_mask;
    uint8_t flags;
    const char *name;        // "subsystem.test_name"
    const char *description;
    test_result_t (*fn)(test_ctx_t *ctx);
} test_desc_t;

struct test_ctx {
    const test_desc_t *test;
    uint8_t op;              // operating point index
    uint32_t clk_khz;        // configured clk_sys at this operating point
    uint32_t errors;         // incremented by rec_error()
    uint32_t errors_logged;
};

// ---- Registry (src/tests/registry.c) ----------------------------------------------
extern const test_desc_t g_tests[];
extern const unsigned g_test_count;

const test_desc_t *registry_find(uint16_t id);
const char *subsys_name(subsys_t s);

// ---- Sequencer ----------------------------------------------------------------------
uint32_t sequencer_stage_count(void);

// Map a stage to (operating point, test). Returns false past the last stage.
bool sequencer_stage(uint32_t stage, unsigned *op, const test_desc_t **test);

// Run stages from run_state->next_stage to the end. `skip_stage` (or UINT32_MAX) is
// recorded as crashed and not run. Returns when all stages are done.
void sequencer_run(uint32_t skip_stage);

// Runtime chip variant (CHIP_* bit). Set once at boot.
void sequencer_set_chip(uint8_t chip_bit);
uint8_t sequencer_chip(void);

#endif
