/*
 * Result records.
 *
 * Tests never format text. They append compact binary records (TLV) to a RAM arena,
 * and writers turn the arena into REPORT.TXT / CSV / JSON afterwards.
 *
 * The arena lives in uninitialized RAM (not cleared at boot), so results collected
 * before a crash survive the watchdog reset. A header with magic, build ID, run nonce
 * and a running CRC32 over the records decides whether the arena can be resumed.
 *
 * Every record carries the test ID and the operating point it was measured at.
 */

#ifndef PICOBENCH_RECORD_H
#define PICOBENCH_RECORD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/stats.h"

typedef enum {
    REC_TEST_END = 1,  // rec_test_end_t
    REC_STATS,         // rec_stats_t
    REC_VALUE,         // rec_value_t
    REC_TEXT,          // rec_text_t + value string
    REC_ERROR,         // rec_error_t
    REC_HIST,          // rec_hist_t + counts
    REC_OPPOINT,       // rec_oppoint_t
    REC_CRASH,         // rec_crash_t
} rec_type_t;

typedef enum {
    UNIT_NONE = 0,
    UNIT_CYCLES,
    UNIT_NS,
    UNIT_US,
    UNIT_MS,
    UNIT_BYTES,
    UNIT_KHZ,
    UNIT_MHZ_X1000,      // milli-hertz (e.g. LED blink frequency)
    UNIT_COUNT,
    UNIT_BOOL,
    UNIT_PPM,
    UNIT_MILLI_C,
    UNIT_MV,
    UNIT_HEX,            // value is shown as hex
    UNIT_KBPS,           // kilobytes per second
} rec_unit_t;

typedef enum {
    TR_PASS = 0,
    TR_FAIL,
    TR_NA,               // not applicable to this chip/board, or hardware not present
    TR_SKIPPED,
    TR_CRASHED,
} test_result_t;

#define REC_LABEL_LEN 32

typedef struct {
    uint16_t type;
    uint16_t len;        // total record length including header, multiple of 8
    uint16_t test_id;
    uint8_t op;          // operating point index
    uint8_t unit;        // rec_unit_t (where applicable)
} rec_hdr_t;

typedef struct {
    rec_hdr_t h;
    uint8_t result;      // test_result_t
    uint8_t pad[3];
    uint32_t duration_us;
    uint32_t errors;
} rec_test_end_t;

typedef struct {
    rec_hdr_t h;
    char label[REC_LABEL_LEN];
    uint32_t work_per_sample;  // e.g. bytes or operations per sample (0 = n/a)
    uint32_t pad;
    stats_t s;           // values in `unit`, normally cycles
} rec_stats_t;

typedef struct {
    rec_hdr_t h;
    char key[REC_LABEL_LEN];
    int64_t value;
} rec_value_t;

typedef struct {
    rec_hdr_t h;
    char key[REC_LABEL_LEN];
    char value[];        // NUL-terminated, padded to 8 bytes
} rec_text_t;

typedef struct {
    rec_hdr_t h;
    char what[REC_LABEL_LEN];
    uint32_t addr;
    uint32_t expected;
    uint32_t actual;
} rec_error_t;

typedef enum { HIST_LOG2 = 0, HIST_LINEAR = 1 } rec_hist_kind_t;

typedef struct {
    rec_hdr_t h;
    char label[REC_LABEL_LEN];
    uint8_t kind;        // rec_hist_kind_t
    uint8_t pad;
    uint16_t nbins;
    uint32_t first;      // linear: value of bin 0
    uint32_t width;      // linear: bin width
    uint32_t counts[];
} rec_hist_t;

typedef struct {
    rec_hdr_t h;         // test_id = 0
    uint32_t requested_khz;
    uint32_t sys_khz;
    uint32_t ref_khz;
    uint32_t peri_khz;
    uint32_t usb_khz;
    uint32_t adc_khz;
    uint32_t aux_khz;
    uint32_t pll_sys_refdiv, pll_sys_fbdiv, pll_sys_pd1, pll_sys_pd2;
    uint32_t pll_usb_refdiv, pll_usb_fbdiv, pll_usb_pd1, pll_usb_pd2;
    uint32_t flash_if_reg;
    uint32_t flash_if_div;
    uint32_t led_blink_mhz;
    bool ok;
} rec_oppoint_t;

typedef struct {
    rec_hdr_t h;
    uint32_t stage;
    uint32_t watchdog_reason;
    uint8_t results_lost;  // RAM arena was invalid; earlier results are gone
    uint8_t pad[3];
} rec_crash_t;

// ---- Arena / run state ----------------------------------------------------------

typedef struct {
    uint32_t run_nonce;
    uint32_t next_stage;   // first stage not yet completed
    uint32_t crash_count;
    uint32_t used;         // bytes of records
    uint32_t record_count;
    uint32_t oppoints_logged;  // bitmask: REC_OPPOINT already written for op i
} run_state_t;

// Start a fresh run (clears the arena).
void record_new_run(uint32_t run_nonce);

// Try to continue the run with this nonce after a reset. False if the arena header or
// CRC does not validate (the caller then starts a new run).
bool record_resume(uint32_t run_nonce);

run_state_t *record_run_state(void);

// Save next_stage etc. into the protected header (recomputes the header CRC).
void record_commit_state(void);

// Iterate records: pass NULL to get the first one. Returns NULL at the end.
const rec_hdr_t *record_next(const rec_hdr_t *prev);

uint32_t record_capacity(void);
bool record_overflowed(void);

// ---- Appending (used by the sequencer and by tests) -------------------------------

struct test_ctx;

void rec_stats(struct test_ctx *ctx, const char *label, rec_unit_t unit,
               uint32_t work_per_sample, const stats_t *s);
void rec_value(struct test_ctx *ctx, const char *key, rec_unit_t unit, int64_t value);
void rec_text(struct test_ctx *ctx, const char *key, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
// Records the first errors of a test in detail; all are counted in ctx->errors.
void rec_error(struct test_ctx *ctx, const char *what, uint32_t addr,
               uint32_t expected, uint32_t actual);
void rec_hist(struct test_ctx *ctx, const char *label, rec_hist_kind_t kind,
              uint32_t first, uint32_t width, uint16_t nbins, const uint32_t *counts);

void rec_test_end(uint16_t test_id, uint8_t op, test_result_t result,
                  uint32_t duration_us, uint32_t errors);
void rec_oppoint(uint8_t op, const rec_oppoint_t *body);
void rec_crash(uint16_t test_id, uint8_t op, uint32_t stage, uint32_t watchdog_reason,
               bool results_lost);

#define REC_MAX_ERRORS_PER_TEST 32

#endif
