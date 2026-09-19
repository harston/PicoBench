/*
 * Human-readable report (becomes REPORT.TXT in phase 2).
 * Every latency table shows min/p50/p95/p99/p99.9/max in cycles and in ns.
 */

#ifndef PICOBENCH_REPORT_TEXT_H
#define PICOBENCH_REPORT_TEXT_H

#include "results/sink.h"

void report_text_write(sink_t *out);

const char *unit_name(unsigned unit);
const char *result_name(unsigned result);

#endif
