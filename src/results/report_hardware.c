/*
 * HARDWARE.TXT - the detected hardware configuration: every record of the inventory
 * tests (TF_INVENTORY), in registry order, one "key : value" line each.
 */

#include "core/sequencer.h"
#include "picobench_version.h"
#include "results/record.h"
#include "results/report_text.h"
#include "results/writers.h"
#include "storage/results_store.h"

static void write_record(sink_t *o, const rec_hdr_t *h) {
    switch (h->type) {
    case REC_VALUE: {
        const rec_value_t *r = (const rec_value_t *)h;
        if (h->unit == UNIT_HEX)
            sink_printf(o, "  %-24s : 0x%llx\n", r->key, (unsigned long long)r->value);
        else if (h->unit == UNIT_BYTES && r->value >= 1024 && r->value % 1024 == 0)
            sink_printf(o, "  %-24s : %lld bytes (%lld KB)\n", r->key, (long long)r->value, (long long)(r->value / 1024));
        else if (h->unit == UNIT_MILLI_C)
            sink_printf(o, "  %-24s : %lld.%01lld degC\n", r->key, (long long)(r->value / 1000),
                        (long long)((r->value < 0 ? -r->value : r->value) % 1000 / 100));
        else
            sink_printf(o, "  %-24s : %lld %s\n", r->key, (long long)r->value, unit_name(h->unit));
        break;
    }
    case REC_TEXT: {
        const rec_text_t *r = (const rec_text_t *)h;
        sink_printf(o, "  %-24s : %s\n", r->key, r->value);
        break;
    }
    case REC_ERROR: {
        const rec_error_t *r = (const rec_error_t *)h;
        sink_printf(o, "  ERROR %-18s : expected 0x%lx, got 0x%lx\n", r->what, (unsigned long)r->expected,
                    (unsigned long)r->actual);
        break;
    }
    case REC_TEST_END: {
        const rec_test_end_t *r = (const rec_test_end_t *)h;
        if (r->result != TR_PASS) sink_printf(o, "  %-24s : %s\n", "result", result_name(r->result));
        break;
    }
    default:
        break;
    }
}

void report_hardware_write(sink_t *o) {
    sink_printf(o, "PicoBench hardware inventory - RUN%04lu\n", (unsigned long)store_next_run_no());
    sink_puts(o, "=======================================\n");
    sink_puts(o, "Everything below is read from the hardware at run time; nothing is assumed from\n"
                 "the build except where marked (board profile is inferred, see its basis).\n");
    for (unsigned i = 0; i < g_test_count; i++) {
        const test_desc_t *t = &g_tests[i];
        if (!(t->flags & TF_INVENTORY)) continue;
        sink_printf(o, "\n[%s] %s\n", t->name, t->description);
        bool any = false;
        for (const rec_hdr_t *h = record_next(NULL); h; h = record_next(h)) {
            if (h->test_id != t->id) continue;
            if (h->type == REC_TEST_END) {
                const rec_test_end_t *r = (const rec_test_end_t *)h;
                if (r->result == TR_NA && !any) sink_puts(o, "  not applicable on this chip\n");
                if (r->result == TR_CRASHED) sink_puts(o, "  TEST_CRASHED - see ERRORS.TXT\n");
            }
            write_record(o, h);
            any = true;
        }
    }
}
