#include "results/sink.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void sink_puts(sink_t *s, const char *str) {
    s->write(s, str, strlen(str));
}

void sink_printf(sink_t *s, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;   // truncated
    s->write(s, buf, (size_t)n);
}
