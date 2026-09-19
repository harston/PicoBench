/*
 * ERRORS.TXT - everything that went wrong in the run, in one place:
 * crashes (watchdog resets), failed/crashed/skipped tests and every logged error.
 */

#include "results/record.h"
#include "results/report_text.h"
#include "results/writers.h"
#include "core/clocks_ctl.h"
#include "core/sequencer.h"
#include "storage/results_store.h"

void report_errors_write(sink_t *o) {
    unsigned count = 0;
    sink_printf(o, "PicoBench errors - RUN%04lu\n", (unsigned long)store_next_run_no());
    sink_puts(o, "=============================\n\n");
    for (const rec_hdr_t *h = record_next(NULL); h; h = record_next(h)) {
        const test_desc_t *t = registry_find(h->test_id);
        const char *tname = t ? t->name : "?";
        unsigned long mhz = (unsigned long)(clocks_op_khz(h->op) / 1000);
        switch (h->type) {
        case REC_CRASH: {
            const rec_crash_t *r = (const rec_crash_t *)h;
            sink_printf(o, "CRASH   stage %lu (%s @ %lu MHz): watchdog reset, reason 0x%lx%s\n",
                        (unsigned long)r->stage,
                        r->stage == sequencer_stage_count() ? "saving the report" : tname, mhz,
                        (unsigned long)r->watchdog_reason,
                        r->results_lost ? ", earlier results lost, run restarted" : "");
            count++;
            break;
        }
        case REC_ERROR: {
            const rec_error_t *r = (const rec_error_t *)h;
            sink_printf(o, "ERROR   [%u] %s @ %lu MHz: %s addr 0x%08lx expected 0x%08lx actual 0x%08lx\n",
                        h->test_id, tname, mhz, r->what, (unsigned long)r->addr,
                        (unsigned long)r->expected, (unsigned long)r->actual);
            count++;
            break;
        }
        case REC_TEST_END: {
            const rec_test_end_t *r = (const rec_test_end_t *)h;
            if (r->result == TR_FAIL || r->result == TR_CRASHED || r->result == TR_SKIPPED) {
                sink_printf(o, "RESULT  [%u] %s @ %lu MHz: %s, %lu error(s)\n", h->test_id, tname, mhz,
                            result_name(r->result), (unsigned long)r->errors);
                count++;
            }
            break;
        }
        default:
            break;
        }
    }
    if (record_overflowed()) {
        sink_puts(o, "ARENA   result arena overflowed: some records are missing\n");
        count++;
    }
    if (count == 0) sink_puts(o, "No errors.\n");
    else sink_printf(o, "\n%u entr%s. Only the first %u errors per test are listed individually;\n"
                        "the per-test error counts above are complete.\n",
                     count, count == 1 ? "y" : "ies", REC_MAX_ERRORS_PER_TEST);
}
