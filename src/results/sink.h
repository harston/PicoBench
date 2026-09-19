/*
 * Output sink for report writers. Phase 1 writes to USB CDC (debug); phase 2 adds a
 * sink that streams into the flash results store.
 */

#ifndef PICOBENCH_SINK_H
#define PICOBENCH_SINK_H

#include <stddef.h>

typedef struct sink {
    void (*write)(struct sink *s, const char *buf, size_t n);
    void *user;
} sink_t;

void sink_puts(sink_t *s, const char *str);
void sink_printf(sink_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
