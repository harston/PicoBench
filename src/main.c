/*
 * PicoBench - Pico Hardware Characterization & Benchmark.
 *
 * Boot flow:
 *   1. Init cycle counter, LED, chip identification.
 *   2. Inspect watchdog scratch: did the previous boot die inside a test stage?
 *        yes -> resume the run from the RAM arena (or restart it if the arena is gone),
 *               record TEST_CRASHED and skip that stage.
 *        no  -> start a new run.
 *   3. Run all stages under the watchdog, LED blinking ~4 Hz.
 *   4. Verdict -> LED PASS/FAIL pattern.
 *   5. (phase 1, debug) expose the text report over USB CDC. Phase 2 replaces this
 *      with the flash results store and the read-only USB disk.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "pico/rand.h"

#include "core/guard.h"
#include "core/led.h"
#include "core/platform.h"
#include "core/sequencer.h"
#include "core/timing.h"
#include "picobench_version.h"
#include "results/record.h"
#include "results/report_text.h"
#include "results/summary.h"

#if PICOBENCH_DEBUG_CDC
#include "pico/bootrom.h"
#include "pico/stdio_usb.h"
#include "hardware/watchdog.h"
#endif

bi_decl(bi_program_feature("Build ID " PICOBENCH_BUILD_ID_STR));
bi_decl(bi_program_feature("Platform " PICOBENCH_PLATFORM));

// Decide between a fresh run and resuming an interrupted one. Returns the stage to
// skip (the one that crashed) or UINT32_MAX. Sets *diagnostic when the run keeps
// crashing and should stop testing.
static uint32_t prepare_run(const guard_boot_info_t *bi, uint32_t *nonce, bool *diagnostic) {
    *diagnostic = false;
    if (!bi->crashed) {
        *nonce = get_rand_32();
        record_new_run(*nonce);
        return UINT32_MAX;
    }

    *nonce = bi->run_nonce;
    bool resumed = record_resume(bi->run_nonce);
    if (!resumed) record_new_run(bi->run_nonce);

    run_state_t *st = record_run_state();
    st->crash_count = bi->crash_count;
    // Resumed: continue exactly at the crashed stage (it will be marked and skipped).
    // Not resumed: earlier results are gone, restart from stage 0 but skip it.
    st->next_stage = resumed ? bi->crashed_stage : 0;
    record_commit_state();

    unsigned op = 0;
    const test_desc_t *t = NULL;
    uint16_t test_id = sequencer_stage(bi->crashed_stage, &op, &t) ? t->id : 0;
    rec_crash(test_id, (uint8_t)op, bi->crashed_stage, bi->watchdog_reason, !resumed);

    if (bi->crash_count >= GUARD_MAX_CRASHES) *diagnostic = true;
    return bi->crashed_stage;
}

#if PICOBENCH_DEBUG_CDC
// Note: newlib FILE functions (fwrite, fflush, ...) do not link with the pqt-gcc
// toolchain (its newlib expects retargetable locks); use the SDK stdio API instead.
static void cdc_write(sink_t *s, const char *buf, size_t n) {
    (void)s;
    stdio_put_string(buf, (int)n, false, true);
}

static void debug_dump(void) {
    sink_t out = {.write = cdc_write};
    report_text_write(&out);
    printf("\n[d] dump again   [r] re-run tests   [b] reboot to BOOTSEL\n");
    stdio_flush();
}

static void debug_console(void) {
    stdio_usb_init();
    bool was_connected = false;
    for (;;) {
        bool connected = stdio_usb_connected();
        if (connected && !was_connected) {
            sleep_ms(300);   // let the host terminal settle
            debug_dump();
        }
        was_connected = connected;
        int ch = getchar_timeout_us(100000);
        if (ch == 'd') debug_dump();
        if (ch == 'r') watchdog_reboot(0, 0, 10);   // scratch is clear -> fresh run
        if (ch == 'b') reset_usb_boot(0, 0);
    }
}
#endif

int main(void) {
    timing_init();
    led_init(PICO_DEFAULT_LED_PIN);
    sequencer_set_chip(platform_chip());

    guard_boot_info_t bi;
    guard_boot(&bi);

    uint32_t nonce;
    bool diagnostic;
    uint32_t skip = prepare_run(&bi, &nonce, &diagnostic);

    led_status(LED_STATUS_TESTING);
    if (!diagnostic) {
        guard_run_start(nonce, record_run_state()->crash_count);
        sequencer_run(skip);
    }
    guard_run_end();

    run_summary_t sum;
    summary_compute(&sum);
    led_status(sum.overall_pass ? LED_STATUS_DONE_PASS : LED_STATUS_DONE_FAIL);

#if PICOBENCH_DEBUG_CDC
    debug_console();
#else
    for (;;) __wfi();
#endif
}
