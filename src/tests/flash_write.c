/*
 * Controlled flash write test.
 *
 * Writes only inside the 64 KB scratch area reserved before the results store
 * (src/storage/flash_layout.h) - never the firmware and never the stored reports.
 * This is not an endurance test: a handful of erase/program/verify cycles, enough to
 * time the operations and prove the data comes back.
 *
 * The SDK's flash routines run from RAM with XIP suspended; interrupts are disabled
 * around them because any handler in flash would fault meanwhile.
 */

#include "tests/tests.h"

#include <string.h>

#include "core/bench.h"
#include "core/guard.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/time.h"
#include "results/record.h"
#include "storage/flash_layout.h"

#define PATTERN_BYTE(off) ((uint8_t)((off) * 31u + 0x5Au))

static uint8_t s_page[FLASH_PAGE_SIZE];

static uint32_t timed_erase(uint32_t off, uint32_t bytes) {
    uint64_t t0 = time_us_64();
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(off, bytes);
    restore_interrupts(irq);
    return (uint32_t)(time_us_64() - t0);
}

static uint32_t timed_program(uint32_t off, const uint8_t *data, uint32_t bytes) {
    uint64_t t0 = time_us_64();
    uint32_t irq = save_and_disable_interrupts();
    flash_range_program(off, data, bytes);
    restore_interrupts(irq);
    return (uint32_t)(time_us_64() - t0);
}

// Compare flash content with `expect` (NULL: expect erased 0xFF). Returns bad bytes.
static uint32_t verify(test_ctx_t *ctx, const char *what, uint32_t off, const uint8_t *expect,
                       uint32_t bytes) {
    xip_cache_invalidate_all();
    const uint8_t *p = flash_xip_ptr(off);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        uint8_t want = expect ? expect[i] : 0xFFu;
        if (p[i] != want) {
            bad++;
            rec_error(ctx, what, off + i, want, p[i]);
        }
    }
    return bad;
}

test_result_t test_flash_write_scratch(test_ctx_t *ctx) {
    const flash_layout_t *l = flash_layout();
    if (!l->valid) {
        rec_text(ctx, "skipped", "flash layout invalid: writing is disabled");
        return TR_NA;
    }
    const uint32_t base = l->scratch_base;
    rec_text(ctx, "area", "%lu KB scratch at 0x%06lx (never the firmware or the reports)",
             (unsigned long)(FLASH_SCRATCH_BYTES / 1024), (unsigned long)base);
    uint32_t errors = 0;

    // 1. Erase one sector and check it really is erased.
    uint32_t us = timed_erase(base, FLASH_SECTOR_SIZE);
    rec_value(ctx, "erase_4k_sector", UNIT_US, us);
    errors += verify(ctx, "erased_sector", base, NULL, FLASH_SECTOR_SIZE);
    guard_keepalive();

    // 2. Program one page, then verify.
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) s_page[i] = PATTERN_BYTE(i);
    us = timed_program(base, s_page, FLASH_PAGE_SIZE);
    rec_value(ctx, "program_256b_page", UNIT_US, us);
    rec_value(ctx, "program_page_rate", UNIT_KBPS, us ? (int64_t)(FLASH_PAGE_SIZE * 1000u / us) : 0);
    errors += verify(ctx, "programmed_page", base, s_page, FLASH_PAGE_SIZE);
    guard_keepalive();

    // 3. Program the rest of the sector in one call (15 pages). The data lives in the
    //    shared sample buffer (a static array here cost the RP2040's record arena 3.75 KB).
    uint8_t *rest = (uint8_t *)g_bench_samples;
    const uint32_t rest_len = FLASH_SECTOR_SIZE - FLASH_PAGE_SIZE;
    _Static_assert(sizeof(g_bench_samples) >= FLASH_SECTOR_SIZE - FLASH_PAGE_SIZE, "sample buffer too small");
    for (uint32_t i = 0; i < rest_len; i++) rest[i] = PATTERN_BYTE(i + FLASH_PAGE_SIZE);
    us = timed_program(base + FLASH_PAGE_SIZE, rest, rest_len);
    rec_value(ctx, "program_rest_of_sector", UNIT_US, us);
    rec_value(ctx, "program_sector_rate", UNIT_KBPS, us ? (int64_t)((uint64_t)rest_len * 1000u / us) : 0);
    errors += verify(ctx, "programmed_rest", base + FLASH_PAGE_SIZE, rest, rest_len);
    guard_keepalive();

    // 4. Erase four sectors at once: the per-sector cost of a larger erase.
    us = timed_erase(base, 4u * FLASH_SECTOR_SIZE);
    rec_value(ctx, "erase_16k_4_sectors", UNIT_US, us);
    rec_value(ctx, "erase_per_sector_us", UNIT_US, us / 4u);
    errors += verify(ctx, "erased_4_sectors", base, NULL, 4u * FLASH_SECTOR_SIZE);
    guard_keepalive();

    // 5. A second program/verify cycle in the same place, to show it repeats.
    us = timed_program(base, s_page, FLASH_PAGE_SIZE);
    rec_value(ctx, "program_256b_page_again", UNIT_US, us);
    errors += verify(ctx, "programmed_page_again", base, s_page, FLASH_PAGE_SIZE);

    // Leave the scratch area erased.
    timed_erase(base, FLASH_SECTOR_SIZE);

    rec_value(ctx, "verify_errors", UNIT_COUNT, errors);
    rec_text(ctx, "note", "erase/program times come from the 1 MHz timer, not the cycle counter: "
                          "these operations take milliseconds and are chip, not CPU, bound");
    return errors ? TR_FAIL : TR_PASS;
}
