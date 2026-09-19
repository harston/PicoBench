/*
 * Per-subsystem verdicts computed from the TEST_END and CRASH records.
 * A verdict never replaces the data: writers print it next to the full tables.
 */

#ifndef PICOBENCH_SUMMARY_H
#define PICOBENCH_SUMMARY_H

#include <stdbool.h>
#include <stdint.h>

#include "core/sequencer.h"

typedef struct {
    uint16_t pass, fail, na, skipped, crashed;
    uint32_t errors;
} subsys_summary_t;

typedef struct {
    subsys_summary_t sub[SUBSYS_COUNT];
    uint32_t crash_records;
    uint32_t tests_run;
    bool arena_overflow;
    bool overall_pass;
} run_summary_t;

void summary_compute(run_summary_t *out);

// "PASS", "FAIL", "N/A" (nothing applicable), "NOT RUN" (no tests in this subsystem yet).
const char *summary_verdict(const subsys_summary_t *s);

#endif
