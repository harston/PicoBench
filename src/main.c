/*
 * PicoBench - Pico Hardware Characterization & Benchmark.
 *
 * Boot flow:
 *   1. Init cycle counter, LED, default clock, flash layout, results store, buttons.
 *   2. Decide what to do:
 *        previous boot died inside a stage  -> resume that run (TEST_CRASHED, skip stage)
 *        re-run requested (button/console)  -> new run
 *        this build has not committed a run yet -> new run
 *        (deleting runs from the disk does not trigger a new run)
 *        otherwise                          -> no tests, just show the reports
 *   3. Run all stages under the watchdog (LED ~4 Hz).
 *   4. Save the report files to flash (LED solid), verified and committed A/B.
 *   5. Start USB: disk PICOBENCH + CDC console. LED shows PASS/FAIL.
 *      Holding BOOTSEL (or KEY on WeAct) for 1 s requests a new run. Deleting
 *      RUNS\RUNxxxx on the disk deletes those runs from flash.
 */

#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "pico/rand.h"
#include "tusb.h"

#include "core/bench.h"
#include "core/button.h"
#include "core/clocks_ctl.h"
#include "core/guard.h"
#include "core/led.h"
#include "core/platform.h"
#include "core/sequencer.h"
#include "core/timing.h"
#include "picobench_version.h"
#include "results/boot_status.h"
#include "results/record.h"
#include "results/summary.h"
#include "results/writers.h"
#include "storage/flash_layout.h"
#include "storage/results_store.h"
#include "usb/console.h"
#include "usb/msc_disk.h"

bi_decl(bi_program_feature("Build ID " PICOBENCH_BUILD_ID_STR));
bi_decl(bi_program_feature("Platform " PICOBENCH_PLATFORM));

#define RERUN_HOLD_MS 1000

// Files stored with every run, in this order.
static const store_file_spec_t s_run_files[] = {
    {"REPORT.TXT", report_text_write},
    {"RESULTS.CSV", report_csv_write},
    {"ERRORS.TXT", report_errors_write},
};

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

// Run (or resume) the tests and save the report. Returns true if a run was committed.
static bool run_and_store(const guard_boot_info_t *bi, bool *pass) {
    uint32_t nonce;
    bool diagnostic;
    uint32_t skip = prepare_run(bi, &nonce, &diagnostic);
    const uint32_t store_stage = sequencer_stage_count();

    led_status(LED_STATUS_TESTING);
    guard_run_start(nonce, record_run_state()->crash_count);
    if (!diagnostic) sequencer_run(skip);

    run_summary_t sum;
    summary_compute(&sum);
    *pass = sum.overall_pass;

    // Saving is a stage too: a hang here is caught by the watchdog. Give up on saving
    // after it crashed twice, rather than looping.
    bool stored = false;
    bool store_keeps_crashing = bi->crashed && bi->crashed_stage == store_stage && bi->crash_count >= 2;
    if (store_keeps_crashing) {
        boot_status_set_save(SAVE_SKIPPED_CRASHES, 0, NULL);
    } else {
        led_status(LED_STATUS_WRITING);
        guard_stage_begin(store_stage);
        uint32_t flags = (sum.overall_pass ? STORE_RUN_PASS : 0) |
                         (sum.crash_records ? STORE_RUN_CRASHES : 0);
        uint32_t run_no = store_next_run_no();
        stored = store_write_run(s_run_files, sizeof(s_run_files) / sizeof(s_run_files[0]),
                                 PICOBENCH_BUILD_ID, nonce, flags);
        guard_stage_end();
        boot_status_set_save(stored ? SAVE_OK : SAVE_FAILED, run_no, store_last_error());
    }
    guard_run_end();
    store_mount();   // re-read and re-verify what is now in flash
    return stored;
}

int main(void) {
    timing_init();
    led_init(PICO_DEFAULT_LED_PIN);
    // Re-apply the default operating point so every point, including the default one,
    // uses the same clk_peri source (set_sys_clock_khz moves it to pll_usb 48 MHz).
    clocks_set_op(clocks_default_op());
    sequencer_set_chip(platform_chip());
    flash_layout_init();
    store_mount();
    button_init();

    guard_boot_info_t bi;
    guard_boot(&bi);

    boot_action_t action = BOOT_NO_TESTS;
    if (bi.crashed) action = BOOT_TESTS_RESUMED;
    else if (bi.rerun_requested) action = BOOT_TESTS_RERUN;
    else if (store_last_build_id() == 0) action = BOOT_TESTS_NO_RESULTS;
    else if (store_last_build_id() != PICOBENCH_BUILD_ID) action = BOOT_TESTS_NEW_BUILD;
    boot_status_set_action(action, bi.crashed_stage, bi.crash_count);

    bool pass;
    if (action != BOOT_NO_TESTS) {
        bool stored = run_and_store(&bi, &pass);
        pass = pass && stored;
    } else {
        const store_run_t *latest = store_latest();
        pass = latest ? (latest->flags & STORE_RUN_PASS) != 0 : true;
    }
    boot_status_set_verdict(pass);
    led_status(pass ? LED_STATUS_DONE_PASS : LED_STATUS_DONE_FAIL);

    // Measurements are over: the sample buffer becomes the RAM overlay of the USB disk.
    uint32_t status_len;
    const char *status = boot_status_render(&status_len);
    msc_disk_build(g_bench_samples, sizeof(g_bench_samples), status, status_len);
    tud_init(0);

    absolute_time_t next_poll = get_absolute_time();
    uint32_t held_ms = 0;
    for (;;) {
        tud_task();
        console_task();
        msc_disk_task();
        if (time_reached(next_poll)) {
            next_poll = make_timeout_time_ms(50);
            if (button_pressed()) {
                if (held_ms == 0) led_status(LED_STATUS_WRITING);   // feedback: solid while held
                held_ms += 50;
                if (held_ms >= RERUN_HOLD_MS) guard_request_rerun();
            } else if (held_ms) {
                held_ms = 0;
                led_status(pass ? LED_STATUS_DONE_PASS : LED_STATUS_DONE_FAIL);
            }
        }
    }
}
