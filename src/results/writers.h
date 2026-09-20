// File writers stored with every run (see main.c for the file list).
#ifndef PICOBENCH_WRITERS_H
#define PICOBENCH_WRITERS_H

#include "results/sink.h"

void report_text_write(sink_t *out);    // REPORT.TXT
void report_csv_write(sink_t *out);     // RESULTS.CSV
void report_errors_write(sink_t *out);  // ERRORS.TXT
void report_hardware_write(sink_t *out); // HARDWARE.TXT

#endif
