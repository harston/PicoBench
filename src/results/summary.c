#include "results/summary.h"

#include <string.h>

void summary_compute(run_summary_t *out) {
    memset(out, 0, sizeof(*out));
    for (const rec_hdr_t *h = record_next(NULL); h; h = record_next(h)) {
        if (h->type == REC_CRASH) {
            out->crash_records++;
            continue;
        }
        if (h->type != REC_TEST_END) continue;
        const rec_test_end_t *r = (const rec_test_end_t *)h;
        const test_desc_t *t = registry_find(h->test_id);
        subsys_summary_t *s = &out->sub[t ? t->subsys : SUBSYS_CORE];
        s->errors += r->errors;
        switch (r->result) {
        case TR_PASS: s->pass++; out->tests_run++; break;
        case TR_FAIL: s->fail++; out->tests_run++; break;
        case TR_NA: s->na++; break;
        case TR_SKIPPED: s->skipped++; break;
        case TR_CRASHED: s->crashed++; break;
        default: break;
        }
    }
    out->arena_overflow = record_overflowed();
    bool pass = out->crash_records == 0 && !out->arena_overflow;
    for (unsigned i = 0; i < SUBSYS_COUNT; i++) {
        const subsys_summary_t *s = &out->sub[i];
        if (s->fail || s->crashed || s->skipped) pass = false;
    }
    out->overall_pass = pass;
}

const char *summary_verdict(const subsys_summary_t *s) {
    if (s->fail || s->crashed || s->skipped) return "FAIL";
    if (s->pass) return "PASS";
    if (s->na) return "N/A";
    return "NOT RUN";
}
