/*
 * Watchdog guard and crash retention.
 *
 * Before each test stage the stage number is written to watchdog scratch registers,
 * which survive a watchdog reset (but not a power cycle). If a stage hangs or
 * crashes, the watchdog resets the chip and the next boot sees the stage that was in
 * progress. It is recorded as TEST_CRASHED and skipped, so the firmware can never
 * boot-loop on the same test.
 *
 * Scratch registers 0-3 only: on RP2350 scratch 4-7 belong to the bootrom, and on
 * RP2040 the SDK uses scratch 4 for its own reboot bookkeeping.
 *
 *   scratch0  GUARD_MAGIC (mixed with the build ID)
 *   scratch1  stage in progress + 1 (0 = none)
 *   scratch2  run nonce (ties the scratch state to the RAM results arena)
 *   scratch3  crash count in this run (kept across resets, so a run that keeps
 *             crashing ends in a diagnostic report instead of looping)
 */

#ifndef PICOBENCH_GUARD_H
#define PICOBENCH_GUARD_H

#include <stdint.h>
#include <stdbool.h>

#include "hardware/watchdog.h"

// Watchdog timeout for every stage. The RP2040 hardware maximum is ~8.3 s; long
// tests must call guard_keepalive() at least this often.
#define GUARD_TIMEOUT_MS 5000u

// Give up and write a diagnostic report after this many crashes in one run.
#define GUARD_MAX_CRASHES 8u

typedef struct {
    bool crashed;              // previous boot died inside a stage
    uint32_t crashed_stage;    // valid when crashed
    uint32_t run_nonce;        // nonce of the interrupted run (valid when crashed)
    uint32_t crash_count;      // crashes so far in the interrupted run, including this one
    uint32_t watchdog_reason;  // raw WATCHDOG REASON register
    bool watchdog_reboot;      // reset was caused by the watchdog
    bool rerun_requested;      // guard_request_rerun() was called before the reset
} guard_boot_info_t;

// Inspect scratch registers and the reset reason. Call once, early in main().
void guard_boot(guard_boot_info_t *out);

// Start a run: store the nonce and crash count, enable the watchdog.
void guard_run_start(uint32_t run_nonce, uint32_t crash_count);

void guard_stage_begin(uint32_t stage);
void guard_stage_end(void);

// Run finished: clear scratch state and stop the watchdog.
void guard_run_end(void);

// Reboot and run the tests again (button / console request).
void guard_request_rerun(void) __attribute__((noreturn));

static inline void guard_keepalive(void) {
    watchdog_update();
}

#endif
