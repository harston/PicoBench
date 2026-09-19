#include "results/boot_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/platform.h"
#include "picobench_version.h"
#include "storage/flash_layout.h"
#include "storage/results_store.h"

static boot_action_t s_action;
static uint32_t s_crashed_stage, s_crash_count;
static save_result_t s_save;
static uint32_t s_save_run;
static char s_save_error[96];
static bool s_pass;

static char s_text[2048];
static uint32_t s_len;

void boot_status_set_action(boot_action_t a, uint32_t crashed_stage, uint32_t crash_count) {
    s_action = a;
    s_crashed_stage = crashed_stage;
    s_crash_count = crash_count;
}

void boot_status_set_save(save_result_t r, uint32_t run_no, const char *error) {
    s_save = r;
    s_save_run = run_no;
    strncpy(s_save_error, error ? error : "", sizeof(s_save_error) - 1);
}

void boot_status_set_verdict(bool pass) {
    s_pass = pass;
}

static void add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void add(const char *fmt, ...) {
    if (s_len >= sizeof(s_text) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_text + s_len, sizeof(s_text) - s_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_len += (uint32_t)n;
    if (s_len > sizeof(s_text) - 1) s_len = sizeof(s_text) - 1;
}

const char *boot_status_render(uint32_t *len) {
    static const char *const actions[] = {
        "no tests - this firmware build was already tested (reports only)",
        "tests run - new firmware build",
        "tests run - no stored results",
        "tests run - re-run requested (button or console)",
        "tests resumed after a crash",
    };
    const flash_layout_t *l = flash_layout();
    s_len = 0;
    add("PicoBench status of the current boot (generated at boot, not stored)\r\n");
    add("====================================================================\r\n\r\n");
    add("Firmware     : %s+%s%s build %s (%s)\r\n", PICOBENCH_VERSION, PICOBENCH_GIT_SHA,
        PICOBENCH_GIT_DIRTY ? "-dirty" : "", PICOBENCH_BUILD_ID_STR, PICOBENCH_PLATFORM);
    add("Chip         : %s rev %u, %s\r\n", platform_chip_name(), platform_chip_revision(),
        platform_arch_name());
    add("Flash        : %lu KB%s, JEDEC %02x %02x %02x\r\n", (unsigned long)(l->flash_size / 1024),
        l->size_detected ? "" : " (NOT detected - fallback)", l->jedec[0], l->jedec[1], l->jedec[2]);
    add("Results area : %lu KB at 0x%06lx%s\r\n", (unsigned long)(l->results_size / 1024),
        (unsigned long)l->results_base, l->valid ? "" : "  INVALID - results cannot be saved");
    add("This boot    : %s\r\n", actions[s_action]);
    if (s_action == BOOT_TESTS_RESUMED)
        add("               crashed in stage %lu, crash %lu of this run\r\n",
            (unsigned long)s_crashed_stage, (unsigned long)s_crash_count);
    switch (s_save) {
    case SAVE_NOT_ATTEMPTED: add("Save         : -\r\n"); break;
    case SAVE_OK: add("Save         : RUN%04lu saved and verified\r\n", (unsigned long)s_save_run); break;
    case SAVE_FAILED: add("Save         : FAILED - %s\r\n", s_save_error); break;
    case SAVE_SKIPPED_CRASHES: add("Save         : FAILED - saving crashed twice, given up\r\n"); break;
    }
    add("Verdict      : %s\r\n", s_pass ? "PASS" : "FAIL (see ERRORS.TXT)");
    add("Stored runs  : %u", store_run_count());
    if (store_run_count())
        add(" (RUN%04lu .. RUN%04lu)", (unsigned long)store_run(0)->run_no,
            (unsigned long)store_latest()->run_no);
    if (store_invalid_runs()) add(", %lu hidden (CRC mismatch)", (unsigned long)store_invalid_runs());
    add("\r\n\r\n");
    add("Deleting RUNS\\RUNxxxx folders (or RUNS itself) on this disk deletes those runs\r\n"
        "from flash. Anything else written to the disk is kept in RAM only and is gone\r\n"
        "after a restart. Deleting results does not make the tests run again.\r\n");
    *len = s_len;
    return s_text;
}

const char *boot_status_text(uint32_t *len) {
    *len = s_len;
    return s_text;
}
