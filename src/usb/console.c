#include "usb/console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/bootrom.h"
#include "pico/time.h"
#include "tusb.h"
#include "core/guard.h"
#include "picobench_version.h"
#include "results/boot_status.h"
#include "storage/flash_layout.h"
#include "storage/results_store.h"

static bool s_was_connected;

// Blocking write; gives up if the host disconnects or stops reading for 2 s.
static void cdc_write(const char *buf, size_t n) {
    absolute_time_t deadline = make_timeout_time_ms(2000);
    while (n) {
        if (!tud_cdc_connected() || time_reached(deadline)) return;
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            tud_task();
            tud_cdc_write_flush();
            continue;
        }
        uint32_t k = n < avail ? (uint32_t)n : avail;
        tud_cdc_write(buf, k);
        buf += k;
        n -= k;
        deadline = make_timeout_time_ms(2000);
    }
    tud_cdc_write_flush();
}

static void cdc_puts(const char *s) {
    cdc_write(s, strlen(s));
}

static void cdc_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cdc_printf(const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) cdc_write(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

// Print a stored text file, converting LF to CRLF for terminals.
static void print_file(const char *name) {
    const store_run_t *run = store_latest();
    if (!run) {
        cdc_puts("No stored runs.\r\n");
        return;
    }
    for (uint32_t i = 0; i < run->nfiles; i++) {
        const store_file_t *f = &run->files[i];
        if (strcmp(f->name, name) != 0) continue;
        const char *p = (const char *)store_file_data(run, f);
        uint32_t start = 0;
        for (uint32_t k = 0; k < f->size; k++) {
            if (p[k] == '\n') {
                cdc_write(p + start, k - start);
                cdc_puts("\r\n");
                start = k + 1;
            }
        }
        if (start < f->size) cdc_write(p + start, f->size - start);
        return;
    }
    cdc_printf("%s not found in RUN%04lu.\r\n", name, (unsigned long)run->run_no);
}

static void list_runs(void) {
    const flash_layout_t *l = flash_layout();
    cdc_printf("Flash %lu KB%s, results area %lu KB at 0x%06lx, directory seq %lu\r\n",
               (unsigned long)(l->flash_size / 1024), l->size_detected ? "" : " (NOT detected, fallback)",
               (unsigned long)(l->results_size / 1024), (unsigned long)l->results_base,
               (unsigned long)store_dir_seq());
    for (unsigned i = 0; i < store_run_count(); i++) {
        const store_run_t *r = store_run(i);
        cdc_printf("  RUN%04lu  build %08lx  %6lu bytes  %s%s%s\r\n", (unsigned long)r->run_no,
                   (unsigned long)r->build_id, (unsigned long)r->blob_len,
                   (r->flags & STORE_RUN_PASS) ? "PASS" : "FAIL",
                   (r->flags & STORE_RUN_CRASHES) ? " (crashes)" : "",
                   (r->flags & STORE_RUN_TRUNCATED) ? " (TRUNCATED)" : "");
    }
    if (store_invalid_runs())
        cdc_printf("  %lu run(s) hidden: data CRC mismatch\r\n", (unsigned long)store_invalid_runs());
}

static void print_status(void) {
    uint32_t len;
    const char *t = boot_status_text(&len);
    cdc_write(t, len);
}

static void help(void) {
    cdc_puts("\r\nPicoBench " PICOBENCH_VERSION "+" PICOBENCH_GIT_SHA " build " PICOBENCH_BUILD_ID_STR "\r\n"
             "  d  REPORT.TXT   e  ERRORS.TXT   s  STATUS.TXT   l  list runs\r\n"
             "  r  run tests again   b  reboot to BOOTSEL   h  help\r\n");
}

void console_task(void) {
    bool connected = tud_cdc_connected();
    if (connected && !s_was_connected) {
        help();
        list_runs();
    }
    s_was_connected = connected;
    if (!connected || !tud_cdc_available()) return;

    char ch = (char)tud_cdc_read_char();
    switch (ch) {
    case 'd': print_file("REPORT.TXT"); break;
    case 'e': print_file("ERRORS.TXT"); break;
    case 's': print_status(); break;
    case 'l': list_runs(); break;
    case 'r':
        cdc_puts("Re-running the tests...\r\n");
        sleep_ms(100);
        guard_request_rerun();
        break;
    case 'b':
        cdc_puts("Rebooting into BOOTSEL...\r\n");
        sleep_ms(100);
        reset_usb_boot(0, 0);
        break;
    case 'h': case '?': help(); break;
    default: break;
    }
}
