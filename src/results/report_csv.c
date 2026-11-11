/*
 * RESULTS.CSV - one row per measurement record, for spreadsheets and scripts.
 *
 * Columns are fixed; cells that do not apply to a record kind are empty.
 * Latency columns hold raw values in `unit`; for unit=cycles the *_ns columns give
 * the same values converted with the operating point's clk_sys.
 */

#include <stdio.h>

#include "core/clocks_ctl.h"
#include "core/platform.h"
#include "core/sequencer.h"
#include "core/timing.h"
#include "picobench_version.h"
#include "results/record.h"
#include "results/report_text.h"
#include "results/writers.h"
#include "storage/results_store.h"

static void ns_cell(char *buf, size_t n, uint32_t cycles, uint32_t khz) {
    uint64_t ps = timing_cycles_to_ps(cycles, khz);
    snprintf(buf, n, "%llu.%03llu", (unsigned long long)(ps / 1000u), (unsigned long long)(ps % 1000u));
}

// CSV-safe text: our keys and values never contain quotes; commas are replaced.
static void csv_text(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = src[i] == ',' ? ';' : src[i];
    dst[i] = '\0';
}

void report_csv_write(sink_t *o) {
    sink_puts(o, "run,build_id,chip,arch,test_id,test,op_mhz,record,label,unit,"
                 "n,min,p50,p95,p99,p99_9,max,mean,"
                 "min_ns,p50_ns,p95_ns,p99_ns,p99_9_ns,max_ns,work_per_sample,value,text\n");
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "%lu,%s,%s,%s", (unsigned long)store_next_run_no(),
             PICOBENCH_BUILD_ID_STR, platform_chip_name(), platform_arch_name());

    for (const rec_hdr_t *h = record_next(NULL); h; h = record_next(h)) {
        const test_desc_t *t = registry_find(h->test_id);
        const char *tname = t ? t->name : "";
        unsigned long mhz = (unsigned long)(clocks_op_khz(h->op) / 1000);
        switch (h->type) {
        case REC_STATS: {
            const rec_stats_t *r = (const rec_stats_t *)h;
            const stats_t *s = &r->s;
            char ns[6][24] = {{0}};
            if (h->unit == UNIT_CYCLES) {
                uint32_t khz = clocks_op_khz(h->op);
                const uint32_t v[6] = {s->min, s->p50, s->p95, s->p99, s->p999, s->max};
                for (int i = 0; i < 6; i++) ns_cell(ns[i], sizeof(ns[i]), v[i], khz);
            }
            uint64_t mean_milli = s->n ? s->sum * 1000u / s->n : 0;
            sink_printf(o, "%s,%u,%s,%lu,stats,%s,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%llu.%03llu,",
                        prefix, h->test_id, tname, mhz, r->label, unit_name(h->unit),
                        (unsigned long)s->n, (unsigned long)s->min, (unsigned long)s->p50,
                        (unsigned long)s->p95, (unsigned long)s->p99, (unsigned long)s->p999,
                        (unsigned long)s->max, (unsigned long long)(mean_milli / 1000),
                        (unsigned long long)(mean_milli % 1000));
            sink_printf(o, "%s,%s,%s,%s,%s,%s,%lu,,%s\n", ns[0], ns[1], ns[2], ns[3], ns[4], ns[5],
                        (unsigned long)r->work_per_sample,
                        (s->flags & STATS_APPROX) ? "approx" : "");
            break;
        }
        case REC_VALUE: {
            const rec_value_t *r = (const rec_value_t *)h;
            sink_printf(o, "%s,%u,%s,%lu,value,%s,%s,,,,,,,,,,,,,,,,%lld,\n", prefix, h->test_id,
                        tname, mhz, r->key, unit_name(h->unit), (long long)r->value);
            break;
        }
        case REC_TEXT: {
            const rec_text_t *r = (const rec_text_t *)h;
            char v[160];
            csv_text(v, sizeof(v), r->value);
            sink_printf(o, "%s,%u,%s,%lu,text,%s,,,,,,,,,,,,,,,,,,%s\n", prefix, h->test_id, tname,
                        mhz, r->key, v);
            break;
        }
        case REC_ERROR: {
            const rec_error_t *r = (const rec_error_t *)h;
            sink_printf(o, "%s,%u,%s,%lu,error,%s,hex,,,,,,,,,,,,,,,,,addr=0x%08lx expected=0x%08lx actual=0x%08lx\n",
                        prefix, h->test_id, tname, mhz, r->what, (unsigned long)r->addr,
                        (unsigned long)r->expected, (unsigned long)r->actual);
            break;
        }
        case REC_TEST_END: {
            const rec_test_end_t *r = (const rec_test_end_t *)h;
            sink_printf(o, "%s,%u,%s,%lu,result,%s,us,,,,,,,,,,,,,,,,%lu,errors=%lu\n", prefix,
                        h->test_id, tname, mhz, result_name(r->result), (unsigned long)r->duration_us,
                        (unsigned long)r->errors);
            break;
        }
        case REC_OPPOINT: {
            const rec_oppoint_t *r = (const rec_oppoint_t *)h;
            sink_printf(o, "%s,0,oppoint,%lu,oppoint,clk_sys,kHz,,,,,,,,,,,,,,,,%lu,"
                        "ref=%lu peri=%lu usb=%lu adc=%lu pll_sys=%lu/%lu/%lu/%lu flash_div=%lu "
                        "flash_rx=%lu vreg_mv=%lu\n",
                        prefix, mhz, (unsigned long)r->sys_khz, (unsigned long)r->ref_khz,
                        (unsigned long)r->peri_khz, (unsigned long)r->usb_khz,
                        (unsigned long)r->adc_khz, (unsigned long)r->pll_sys_refdiv,
                        (unsigned long)r->pll_sys_fbdiv, (unsigned long)r->pll_sys_pd1,
                        (unsigned long)r->pll_sys_pd2, (unsigned long)r->flash_if_div,
                        (unsigned long)r->flash_if_rxdelay, (unsigned long)r->vreg_mv);
            break;
        }
        case REC_CRASH: {
            const rec_crash_t *r = (const rec_crash_t *)h;
            sink_printf(o, "%s,%u,%s,%lu,crash,stage,,,,,,,,,,,,,,,,,%lu,watchdog_reason=0x%lx%s\n",
                        prefix, h->test_id, tname, mhz, (unsigned long)r->stage,
                        (unsigned long)r->watchdog_reason, r->results_lost ? " results_lost" : "");
            break;
        }
        default:
            break;
        }
    }
}
