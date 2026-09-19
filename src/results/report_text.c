#include "results/report_text.h"

#include <stdio.h>

#include "core/clocks_ctl.h"
#include "core/platform.h"
#include "core/sequencer.h"
#include "core/timing.h"
#include "pico/version.h"
#include "picobench_version.h"
#include "results/record.h"
#include "results/summary.h"

const char *unit_name(unsigned unit) {
    switch (unit) {
    case UNIT_CYCLES: return "cycles";
    case UNIT_NS: return "ns";
    case UNIT_US: return "us";
    case UNIT_MS: return "ms";
    case UNIT_BYTES: return "bytes";
    case UNIT_KHZ: return "kHz";
    case UNIT_MHZ_X1000: return "mHz";
    case UNIT_COUNT: return "";
    case UNIT_BOOL: return "bool";
    case UNIT_PPM: return "ppm";
    case UNIT_MILLI_C: return "mdegC";
    case UNIT_MV: return "mV";
    case UNIT_HEX: return "hex";
    case UNIT_KBPS: return "kB/s";
    default: return "";
    }
}

const char *result_name(unsigned result) {
    switch (result) {
    case TR_PASS: return "PASS";
    case TR_FAIL: return "FAIL";
    case TR_NA: return "N/A";
    case TR_SKIPPED: return "SKIPPED";
    case TR_CRASHED: return "TEST_CRASHED";
    default: return "?";
    }
}

// "12.345" style: value / 1000 with three decimals.
static void fmt_milli(char *buf, size_t n, uint64_t milli) {
    snprintf(buf, n, "%llu.%03llu", (unsigned long long)(milli / 1000u),
             (unsigned long long)(milli % 1000u));
}

static void write_header(sink_t *o) {
    const run_state_t *st = record_run_state();
    sink_puts(o, "PicoBench - Pico Hardware Characterization & Benchmark\n");
    sink_puts(o, "=====================================================\n\n");
    sink_printf(o, "Firmware    : %s+%s%s  build %s  (%s)\n", PICOBENCH_VERSION, PICOBENCH_GIT_SHA,
                PICOBENCH_GIT_DIRTY ? "-dirty" : "", PICOBENCH_BUILD_ID_STR, PICOBENCH_BUILD_TIME);
    sink_printf(o, "Platform    : %s  (Pico SDK %s)\n", PICOBENCH_PLATFORM, PICO_SDK_VERSION_STRING);
    sink_printf(o, "Chip        : %s rev %u  (CHIP_ID 0x%08lx), running on %s\n", platform_chip_name(),
                platform_chip_revision(), (unsigned long)platform_chip_id_raw(), platform_arch_name());
    sink_printf(o, "Cycle timer : %s, read overhead %lu cycles (subtracted)\n", TIMING_COUNTER_NAME,
                (unsigned long)timing_overhead());
    sink_printf(o, "Run         : nonce %08lx, %lu/%lu stages, %lu records, %lu/%lu bytes, %lu crash(es)\n",
                (unsigned long)st->run_nonce, (unsigned long)st->next_stage,
                (unsigned long)sequencer_stage_count(), (unsigned long)st->record_count,
                (unsigned long)st->used, (unsigned long)record_capacity(), (unsigned long)st->crash_count);
    sink_puts(o, "Timing note : all timings are internal (CPU/bus/peripheral). Electrical timing\n"
                 "              (rise time, PCB propagation, input thresholds) is NOT measured.\n\n");
}

static void write_oppoints(sink_t *o) {
    sink_puts(o, "Operating points (measured by the frequency counter)\n");
    sink_puts(o, "  op  req MHz   clk_sys   clk_ref  clk_peri   clk_usb   clk_adc  PLL_SYS ref/fb/pd1/pd2  flash div  LED Hz\n");
    for (const rec_hdr_t *h = record_next(NULL); h; h = record_next(h)) {
        if (h->type != REC_OPPOINT) continue;
        const rec_oppoint_t *r = (const rec_oppoint_t *)h;
        char led[16];
        fmt_milli(led, sizeof(led), r->led_blink_mhz);
        sink_printf(o, "  %2u  %7lu  %8lu  %8lu  %8lu  %8lu  %8lu  %3lu/%3lu/%lu/%lu  %9lu  %6s%s\n",
                    h->op, (unsigned long)(r->requested_khz / 1000), (unsigned long)r->sys_khz,
                    (unsigned long)r->ref_khz, (unsigned long)r->peri_khz, (unsigned long)r->usb_khz,
                    (unsigned long)r->adc_khz, (unsigned long)r->pll_sys_refdiv,
                    (unsigned long)r->pll_sys_fbdiv, (unsigned long)r->pll_sys_pd1,
                    (unsigned long)r->pll_sys_pd2, (unsigned long)r->flash_if_div, led,
                    r->ok ? "" : "  (FAILED TO SET)");
    }
    sink_puts(o, "  (clock values in kHz)\n\n");
}

static void write_stats(sink_t *o, const rec_stats_t *r) {
    const stats_t *s = &r->s;
    uint64_t mean_milli = s->n ? (s->sum * 1000u) / s->n : 0;
    char mean[24];
    fmt_milli(mean, sizeof(mean), mean_milli);
    sink_printf(o, "    %-24s n=%-6lu min %lu  p50 %lu  p95 %lu  p99 %lu  p99.9 %lu  max %lu  mean %s %s%s\n",
                r->label, (unsigned long)s->n, (unsigned long)s->min, (unsigned long)s->p50,
                (unsigned long)s->p95, (unsigned long)s->p99, (unsigned long)s->p999,
                (unsigned long)s->max, mean, unit_name(r->h.unit),
                (s->flags & STATS_APPROX) ? "  (approx.)" : "");
    if (r->h.unit == UNIT_CYCLES) {
        uint32_t khz = clocks_op_khz(r->h.op);
        char v[6][24];
        const uint32_t vals[6] = {s->min, s->p50, s->p95, s->p99, s->p999, s->max};
        for (int i = 0; i < 6; i++) fmt_milli(v[i], sizeof(v[i]), timing_cycles_to_ps(vals[i], khz));
        sink_printf(o, "    %-24s ns     min %s  p50 %s  p95 %s  p99 %s  p99.9 %s  max %s\n", "",
                    v[0], v[1], v[2], v[3], v[4], v[5]);
        if (r->work_per_sample) {
            char per[24];
            fmt_milli(per, sizeof(per), (uint64_t)s->p50 * 1000u / r->work_per_sample);
            sink_printf(o, "    %-24s p50 %s cycles per unit of work (%lu units/sample)\n", "", per,
                        (unsigned long)r->work_per_sample);
        }
    }
}

static void write_tests(sink_t *o) {
    sink_puts(o, "Tests\n");
    int cur_id = -1, cur_op = -1;
    for (const rec_hdr_t *h = record_next(NULL); h; h = record_next(h)) {
        if (h->type == REC_OPPOINT) continue;
        if (h->type == REC_CRASH) {
            const rec_crash_t *c = (const rec_crash_t *)h;
            const test_desc_t *t = registry_find(h->test_id);
            sink_printf(o, "  !! TEST_CRASHED: stage %lu (%s @ op %u), watchdog reason 0x%lx%s\n",
                        (unsigned long)c->stage, t ? t->name : "?", h->op,
                        (unsigned long)c->watchdog_reason,
                        c->results_lost ? " - earlier results were lost, run restarted" : "");
            continue;
        }
        if (h->test_id != cur_id || h->op != cur_op) {
            const test_desc_t *t = registry_find(h->test_id);
            sink_printf(o, "  [%u] %s @ %lu MHz\n", h->test_id, t ? t->name : "?",
                        (unsigned long)(clocks_op_khz(h->op) / 1000));
            cur_id = h->test_id;
            cur_op = h->op;
        }
        switch (h->type) {
        case REC_STATS:
            write_stats(o, (const rec_stats_t *)h);
            break;
        case REC_VALUE: {
            const rec_value_t *r = (const rec_value_t *)h;
            if (h->unit == UNIT_HEX)
                sink_printf(o, "    %-24s 0x%llx\n", r->key, (unsigned long long)r->value);
            else
                sink_printf(o, "    %-24s %lld %s\n", r->key, (long long)r->value, unit_name(h->unit));
            break;
        }
        case REC_TEXT: {
            const rec_text_t *r = (const rec_text_t *)h;
            sink_printf(o, "    %-24s %s\n", r->key, r->value);
            break;
        }
        case REC_ERROR: {
            const rec_error_t *r = (const rec_error_t *)h;
            sink_printf(o, "    ERROR %-18s addr 0x%08lx expected 0x%08lx actual 0x%08lx\n", r->what,
                        (unsigned long)r->addr, (unsigned long)r->expected, (unsigned long)r->actual);
            break;
        }
        case REC_HIST: {
            const rec_hist_t *r = (const rec_hist_t *)h;
            sink_printf(o, "    histogram %s (%s, %u bins)\n", r->label,
                        r->kind == HIST_LOG2 ? "log2" : "linear", r->nbins);
            for (unsigned b = 0; b < r->nbins; b++) {
                if (!r->counts[b]) continue;
                if (r->kind == HIST_LOG2)
                    sink_printf(o, "      < 2^%-2u : %lu\n", b, (unsigned long)r->counts[b]);
                else
                    sink_printf(o, "      %6lu : %lu\n", (unsigned long)(r->first + b * r->width),
                                (unsigned long)r->counts[b]);
            }
            break;
        }
        case REC_TEST_END: {
            const rec_test_end_t *r = (const rec_test_end_t *)h;
            char ms[24];
            fmt_milli(ms, sizeof(ms), r->duration_us);
            sink_printf(o, "    => %s  (%s ms, %lu error(s))\n", result_name(r->result), ms,
                        (unsigned long)r->errors);
            cur_id = -1;
            break;
        }
        default:
            break;
        }
    }
    sink_puts(o, "\n");
}

static void write_verdicts(sink_t *o) {
    run_summary_t sum;
    summary_compute(&sum);
    sink_puts(o, "Verdict per subsystem (see tables above: PASS never hides latency values)\n");
    for (unsigned i = 0; i < SUBSYS_COUNT; i++) {
        const subsys_summary_t *s = &sum.sub[i];
        sink_printf(o, "  %-10s %-8s pass %u  fail %u  crashed %u  skipped %u  n/a %u  errors %lu\n",
                    subsys_name((subsys_t)i), summary_verdict(s), s->pass, s->fail, s->crashed,
                    s->skipped, s->na, (unsigned long)s->errors);
    }
    sink_printf(o, "  crash records: %lu%s\n", (unsigned long)sum.crash_records,
                sum.arena_overflow ? "   RESULT ARENA OVERFLOWED - some records are missing" : "");
    sink_printf(o, "\nOVERALL: %s\n", sum.overall_pass ? "PASS" : "FAIL");
}

void report_text_write(sink_t *out) {
    write_header(out);
    write_oppoints(out);
    write_tests(out);
    write_verdicts(out);
}
