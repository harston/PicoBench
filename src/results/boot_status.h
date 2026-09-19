/*
 * STATUS.TXT - what this boot did, generated in RAM at every boot (never stored).
 * Makes problems visible on the disk itself: e.g. a report that could not be saved.
 */

#ifndef PICOBENCH_BOOT_STATUS_H
#define PICOBENCH_BOOT_STATUS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BOOT_NO_TESTS = 0,       // build already tested, reports only
    BOOT_TESTS_NEW_BUILD,
    BOOT_TESTS_NO_RESULTS,   // store empty and no build recorded
    BOOT_TESTS_RERUN,        // button / console request
    BOOT_TESTS_RESUMED,      // previous boot crashed inside a stage
} boot_action_t;

typedef enum {
    SAVE_NOT_ATTEMPTED = 0,
    SAVE_OK,
    SAVE_FAILED,
    SAVE_SKIPPED_CRASHES,    // saving crashed twice, given up
} save_result_t;

void boot_status_set_action(boot_action_t a, uint32_t crashed_stage, uint32_t crash_count);
void boot_status_set_save(save_result_t r, uint32_t run_no, const char *error);
void boot_status_set_verdict(bool pass);

// Render STATUS.TXT once (call after the above). Returns the text and its length.
const char *boot_status_render(uint32_t *len);

// The text rendered last (the USB disk serves this buffer; it must not change later).
const char *boot_status_text(uint32_t *len);

#endif
