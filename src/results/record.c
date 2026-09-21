#include "results/record.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico.h"
#include "core/sequencer.h"
#include "picobench_version.h"
#include "results/crc32.h"

// Sized for a full run: phase 4 alone produces several hundred records, and the
// sweeps multiply the ones flagged TF_SWEEP by the number of operating points.
// Phase 5 already fills 40 KB of the RP2040 arena (81 %), so give both chips headroom
// for the phases still to come.
#if PICO_RP2040
#define ARENA_BYTES (64 * 1024)
#else
#define ARENA_BYTES (128 * 1024)
#endif

#define ARENA_MAGIC 0x50424152u   // "PBAR"

typedef struct {
    uint32_t magic;
    uint32_t build_id;
    run_state_t st;
    uint32_t data_crc;     // CRC32 over data[0..st.used)
    uint32_t overflow;
    uint32_t hdr_crc;      // CRC32 over everything above
} arena_hdr_t;

typedef struct {
    arena_hdr_t hdr;
    uint8_t data[ARENA_BYTES] __attribute__((aligned(8)));
} arena_t;

// Not zeroed at boot: survives a watchdog reset.
static arena_t __uninitialized_ram(s_arena) __attribute__((aligned(8)));

static uint32_t hdr_crc(const arena_hdr_t *h) {
    return crc32_update(0, h, offsetof(arena_hdr_t, hdr_crc));
}

static void seal(void) {
    s_arena.hdr.hdr_crc = hdr_crc(&s_arena.hdr);
}

void record_new_run(uint32_t run_nonce) {
    memset(&s_arena.hdr, 0, sizeof(s_arena.hdr));
    s_arena.hdr.magic = ARENA_MAGIC;
    s_arena.hdr.build_id = PICOBENCH_BUILD_ID;
    s_arena.hdr.st.run_nonce = run_nonce;
    s_arena.hdr.data_crc = 0;
    seal();
}

bool record_resume(uint32_t run_nonce) {
    const arena_hdr_t *h = &s_arena.hdr;
    if (h->magic != ARENA_MAGIC) return false;
    if (h->hdr_crc != hdr_crc(h)) return false;
    if (h->build_id != PICOBENCH_BUILD_ID) return false;
    if (h->st.run_nonce != run_nonce) return false;
    if (h->st.used > ARENA_BYTES) return false;
    if (crc32_update(0, s_arena.data, h->st.used) != h->data_crc) return false;
    return true;
}

run_state_t *record_run_state(void) {
    return &s_arena.hdr.st;
}

void record_commit_state(void) {
    seal();
}

uint32_t record_capacity(void) {
    return ARENA_BYTES;
}

bool record_overflowed(void) {
    return s_arena.hdr.overflow != 0;
}

const rec_hdr_t *record_next(const rec_hdr_t *prev) {
    uint32_t off = prev ? (uint32_t)((const uint8_t *)prev - s_arena.data) + prev->len : 0;
    if (off + sizeof(rec_hdr_t) > s_arena.hdr.st.used) return NULL;
    const rec_hdr_t *r = (const rec_hdr_t *)(s_arena.data + off);
    if (r->len < sizeof(rec_hdr_t) || off + r->len > s_arena.hdr.st.used) return NULL;
    return r;
}

// Reserve `len` bytes (rounded up to 8: records hold 64-bit fields) and fill the header. NULL when full.
static void *alloc(rec_type_t type, uint32_t len, uint16_t test_id, uint8_t op, uint8_t unit) {
    len = (len + 7u) & ~7u;
    run_state_t *st = &s_arena.hdr.st;
    if (len > 0xFFFFu || st->used + len > ARENA_BYTES) {
        s_arena.hdr.overflow++;
        seal();
        return NULL;
    }
    rec_hdr_t *h = (rec_hdr_t *)(s_arena.data + st->used);
    memset(h, 0, len);
    h->type = (uint16_t)type;
    h->len = (uint16_t)len;
    h->test_id = test_id;
    h->op = op;
    h->unit = unit;
    return h;
}

static void finish(void *rec) {
    rec_hdr_t *h = rec;
    run_state_t *st = &s_arena.hdr.st;
    s_arena.hdr.data_crc = crc32_update(s_arena.hdr.data_crc, h, h->len);
    st->used += h->len;
    st->record_count++;
    seal();
}

static void copy_label(char *dst, const char *src) {
    strncpy(dst, src ? src : "", REC_LABEL_LEN - 1);
    dst[REC_LABEL_LEN - 1] = '\0';
}

void rec_stats(test_ctx_t *ctx, const char *label, rec_unit_t unit,
               uint32_t work_per_sample, const stats_t *s) {
    rec_stats_t *r = alloc(REC_STATS, sizeof(*r), ctx->test->id, ctx->op, (uint8_t)unit);
    if (!r) return;
    copy_label(r->label, label);
    r->work_per_sample = work_per_sample;
    r->s = *s;
    finish(r);
}

void rec_value(test_ctx_t *ctx, const char *key, rec_unit_t unit, int64_t value) {
    rec_value_t *r = alloc(REC_VALUE, sizeof(*r), ctx->test->id, ctx->op, (uint8_t)unit);
    if (!r) return;
    copy_label(r->key, key);
    r->value = value;
    finish(r);
}

void rec_text(test_ctx_t *ctx, const char *key, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t vlen = strlen(buf) + 1;
    rec_text_t *r = alloc(REC_TEXT, (uint32_t)(sizeof(*r) + vlen), ctx->test->id, ctx->op, UNIT_NONE);
    if (!r) return;
    copy_label(r->key, key);
    memcpy(r->value, buf, vlen);
    finish(r);
}

void rec_error(test_ctx_t *ctx, const char *what, uint32_t addr,
               uint32_t expected, uint32_t actual) {
    ctx->errors++;
    if (ctx->errors_logged >= REC_MAX_ERRORS_PER_TEST) return;
    ctx->errors_logged++;
    rec_error_t *r = alloc(REC_ERROR, sizeof(*r), ctx->test->id, ctx->op, UNIT_HEX);
    if (!r) return;
    copy_label(r->what, what);
    r->addr = addr;
    r->expected = expected;
    r->actual = actual;
    finish(r);
}

void rec_hist(test_ctx_t *ctx, const char *label, rec_hist_kind_t kind,
              uint32_t first, uint32_t width, uint16_t nbins, const uint32_t *counts) {
    rec_hist_t *r = alloc(REC_HIST, (uint32_t)(sizeof(*r) + nbins * sizeof(uint32_t)),
                          ctx->test->id, ctx->op, UNIT_COUNT);
    if (!r) return;
    copy_label(r->label, label);
    r->kind = (uint8_t)kind;
    r->nbins = nbins;
    r->first = first;
    r->width = width;
    memcpy(r->counts, counts, nbins * sizeof(uint32_t));
    finish(r);
}

void rec_test_end(uint16_t test_id, uint8_t op, test_result_t result,
                  uint32_t duration_us, uint32_t errors) {
    rec_test_end_t *r = alloc(REC_TEST_END, sizeof(*r), test_id, op, UNIT_NONE);
    if (!r) return;
    r->result = (uint8_t)result;
    r->duration_us = duration_us;
    r->errors = errors;
    finish(r);
}

void rec_oppoint(uint8_t op, const rec_oppoint_t *body) {
    rec_oppoint_t *r = alloc(REC_OPPOINT, sizeof(*r), 0, op, UNIT_KHZ);
    if (!r) return;
    rec_hdr_t h = r->h;
    *r = *body;
    r->h = h;
    finish(r);
}

void rec_crash(uint16_t test_id, uint8_t op, uint32_t stage, uint32_t watchdog_reason,
               bool results_lost) {
    rec_crash_t *r = alloc(REC_CRASH, sizeof(*r), test_id, op, UNIT_NONE);
    if (!r) return;
    r->stage = stage;
    r->watchdog_reason = watchdog_reason;
    r->results_lost = results_lost ? 1 : 0;
    finish(r);
}
