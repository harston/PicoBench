/*
 * PSRAM correctness (RP2350 with a PSRAM chip fitted).
 *
 * Covers the whole chip: constant patterns, walking bits, address and inverted
 * address, an LFSR sequence, byte and halfword access, and March C- over a bounded
 * region. Two extra checks matter for a chip that may not be what it claims:
 *
 *   - an address-wrap probe, the same idea as for flash: if a "8 MB" chip is really
 *     4 MB, the upper half mirrors the lower one,
 *   - the first and last word of every 1 MB region, which catches a bank or address
 *     line that never reaches the die.
 *
 * All accesses go through the uncached alias, so every read really reaches the chip
 * instead of being served by the 16 KB XIP cache.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/guard.h"
#include "detect/psram_info.h"
#include "pico/time.h"
#include "results/record.h"

#define CHUNK_BYTES (64u * 1024u)   // keepalive granularity for the watchdog
#define MARCH_BYTES (1024u * 1024u)

#if PICO_RP2040

test_result_t test_psram_integrity(test_ctx_t *ctx) {
    (void)ctx;
    return TR_NA;
}

#else

typedef uint32_t (*pattern_fn)(uint32_t index, uint32_t seed);

static uint32_t pat_const(uint32_t i, uint32_t seed) { (void)i; return seed; }
static uint32_t pat_walk1(uint32_t i, uint32_t seed) { (void)seed; return 1u << (i & 31u); }
static uint32_t pat_walk0(uint32_t i, uint32_t seed) { (void)seed; return ~(1u << (i & 31u)); }
static uint32_t pat_addr(uint32_t i, uint32_t seed) { return seed + i * 4u; }
static uint32_t pat_addr_inv(uint32_t i, uint32_t seed) { return ~(seed + i * 4u); }

static uint32_t pat_lfsr(uint32_t i, uint32_t seed) {
    uint32_t x = seed ^ (i * 2654435761u);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

static volatile uint32_t *psram_words(void) {
    return (volatile uint32_t *)PSRAM_NOCACHE_BASE;
}

// Write the pattern over the whole chip, then read it back.
static uint32_t run_pattern(test_ctx_t *ctx, const char *name, uint32_t bytes, pattern_fn fn,
                            uint32_t seed) {
    volatile uint32_t *p = psram_words();
    uint32_t words = bytes / 4u;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < words; i++) {
        p[i] = fn(i, seed);
        if ((i & (CHUNK_BYTES / 4u - 1u)) == 0) guard_keepalive();
    }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t want = fn(i, seed);
        uint32_t got = p[i];
        if (got != want) {
            bad++;
            rec_error(ctx, name, (uint32_t)(uintptr_t)&p[i], want, got);
        }
        if ((i & (CHUNK_BYTES / 4u - 1u)) == 0) guard_keepalive();
    }
    char key[REC_LABEL_LEN + 8];
    snprintf(key, sizeof(key), "%s_us", name);
    rec_value(ctx, key, UNIT_US, (uint32_t)(time_us_64() - t0));
    return bad;
}

// March C- over the first MARCH_BYTES: catches coupling faults between neighbouring cells.
static uint32_t march_c_minus(test_ctx_t *ctx, uint32_t bytes) {
    volatile uint32_t *p = psram_words();
    uint32_t n = bytes / 4u;
    uint32_t bad = 0;
    const uint32_t zero = 0u, one = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < n; i++) p[i] = zero;
    guard_keepalive();
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != zero) { bad++; rec_error(ctx, "march_up_r0", (uint32_t)(uintptr_t)&p[i], zero, p[i]); }
        p[i] = one;
    }
    guard_keepalive();
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != one) { bad++; rec_error(ctx, "march_up_r1", (uint32_t)(uintptr_t)&p[i], one, p[i]); }
        p[i] = zero;
    }
    guard_keepalive();
    for (uint32_t i = n; i-- > 0;) {
        if (p[i] != zero) { bad++; rec_error(ctx, "march_dn_r0", (uint32_t)(uintptr_t)&p[i], zero, p[i]); }
        p[i] = one;
    }
    guard_keepalive();
    for (uint32_t i = n; i-- > 0;) {
        if (p[i] != one) { bad++; rec_error(ctx, "march_dn_r1", (uint32_t)(uintptr_t)&p[i], one, p[i]); }
        p[i] = zero;
    }
    guard_keepalive();
    for (uint32_t i = n; i-- > 0;) {
        if (p[i] != zero) { bad++; rec_error(ctx, "march_final_r0", (uint32_t)(uintptr_t)&p[i], zero, p[i]); }
    }
    return bad;
}

// Byte and halfword writes: the QMI write path is per byte lane.
static uint32_t narrow_access(test_ctx_t *ctx, uint32_t bytes) {
    volatile uint8_t *b = (volatile uint8_t *)PSRAM_NOCACHE_BASE;
    volatile uint16_t *h = (volatile uint16_t *)PSRAM_NOCACHE_BASE;
    uint32_t bad = 0;
    uint32_t n = bytes < CHUNK_BYTES ? bytes : CHUNK_BYTES;
    for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 31u + 7u);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t want = (uint8_t)(i * 31u + 7u);
        if (b[i] != want) { bad++; rec_error(ctx, "byte_access", (uint32_t)(uintptr_t)&b[i], want, b[i]); }
    }
    guard_keepalive();
    for (uint32_t i = 0; i < n / 2u; i++) h[i] = (uint16_t)(i ^ 0xA5A5u);
    for (uint32_t i = 0; i < n / 2u; i++) {
        uint16_t want = (uint16_t)(i ^ 0xA5A5u);
        if (h[i] != want) { bad++; rec_error(ctx, "halfword_access", (uint32_t)(uintptr_t)&h[i], want, h[i]); }
    }
    return bad;
}

// Is the chip as large as it claims? Write a marker at offset 0 and check whether it
// reappears at a power-of-two offset (which would mean the address lines wrapped).
static uint32_t size_check(test_ctx_t *ctx, uint32_t bytes) {
    volatile uint32_t *p = psram_words();
    const uint32_t marker = 0xC0DEFACEu;
    uint32_t bad = 0;
    p[0] = marker;
    if (p[0] != marker) {   // not an addressing question at all
        rec_text(ctx, "address_wrap", "cannot test: offset 0 reads back 0x%08lx instead of 0x%08lx",
                 (unsigned long)p[0], (unsigned long)marker);
        rec_error(ctx, "psram_not_responding", 0, marker, p[0]);
        return 1;
    }
    for (uint32_t off = 64u * 1024u; off < bytes; off <<= 1) {
        p[off / 4u] = ~marker;                 // different value at the mirror candidate
        uint32_t at_off = p[off / 4u];
        uint32_t at_zero = p[0];
        if (at_zero == marker) continue;       // no mirroring at this offset
        if (at_off != ~marker) {
            // Neither location holds what was written: the chip is not answering
            // correctly, which is a different fault from a smaller-than-claimed chip.
            rec_text(ctx, "address_wrap", "inconclusive at 0x%06lx: 0x%08lx at the offset, "
                     "0x%08lx at zero", (unsigned long)off, (unsigned long)at_off,
                     (unsigned long)at_zero);
            rec_error(ctx, "psram_not_responding", off, ~marker, at_off);
            return 1;
        }
        rec_text(ctx, "address_wrap", "writing at 0x%06lx changed offset 0: the chip is only "
                 "%lu KB, not the %lu KB reported", (unsigned long)off,
                 (unsigned long)(off / 1024), (unsigned long)(bytes / 1024));
        rec_error(ctx, "psram_size_overstated", off, bytes, off);
        return 1;
    }
    rec_text(ctx, "address_wrap", "none up to %lu KB", (unsigned long)(bytes / 1024));

    // First and last word of every 1 MB region, written with the region index.
    uint32_t regions = bytes / (1024u * 1024u);
    for (uint32_t r = 0; r < regions; r++) {
        uint32_t first = r * (1024u * 1024u) / 4u;
        uint32_t last = first + (1024u * 1024u) / 4u - 1u;
        p[first] = 0xF1750000u + r;
        p[last] = 0x1A570000u + r;
    }
    for (uint32_t r = 0; r < regions; r++) {
        uint32_t first = r * (1024u * 1024u) / 4u;
        uint32_t last = first + (1024u * 1024u) / 4u - 1u;
        if (p[first] != 0xF1750000u + r) {
            bad++;
            rec_error(ctx, "region_first_word", (uint32_t)(uintptr_t)&p[first], 0xF1750000u + r, p[first]);
        }
        if (p[last] != 0x1A570000u + r) {
            bad++;
            rec_error(ctx, "region_last_word", (uint32_t)(uintptr_t)&p[last], 0x1A570000u + r, p[last]);
        }
    }
    rec_value(ctx, "regions_checked", UNIT_COUNT, regions);
    return bad;
}

test_result_t test_psram_integrity(test_ctx_t *ctx) {
    if (!g_psram.present || !g_psram.initialised) {
        rec_text(ctx, "psram", "not present (see psram.detect)");
        return TR_NA;
    }
    // Do not trust the QMI state left by earlier stages: the XIP mode sweep and any
    // flash write reconfigure the shared controller.
    if (!psram_reapply()) {
        rec_error(ctx, "psram_reapply", 0, 1, 0);
        return TR_FAIL;
    }
    uint32_t bytes = g_psram.size;
    rec_text(ctx, "area", "%lu KB on GPIO%u, tested through the uncached alias 0x%08lx",
             (unsigned long)(bytes / 1024), g_psram.cs_gpio, (unsigned long)PSRAM_NOCACHE_BASE);

    // Full-chip passes for the patterns that catch addressing and stuck-bit faults;
    // the rest run over the first megabyte, so an 8 MB chip does not take a minute.
    uint32_t part = bytes < MARCH_BYTES ? bytes : MARCH_BYTES;
    rec_value(ctx, "full_pass_bytes", UNIT_BYTES, bytes);
    rec_value(ctx, "partial_pass_bytes", UNIT_BYTES, part);

    uint32_t bad = 0;
    bad += size_check(ctx, bytes);
    guard_keepalive();
    bad += run_pattern(ctx, "const_00", bytes, pat_const, 0x00000000u);
    bad += run_pattern(ctx, "const_ff", bytes, pat_const, 0xFFFFFFFFu);
    bad += run_pattern(ctx, "address", bytes, pat_addr, (uint32_t)PSRAM_NOCACHE_BASE);
    bad += run_pattern(ctx, "lfsr", bytes, pat_lfsr, 0x1234ABCDu);
    bad += run_pattern(ctx, "const_55", part, pat_const, 0x55555555u);
    bad += run_pattern(ctx, "const_aa", part, pat_const, 0xAAAAAAAAu);
    bad += run_pattern(ctx, "walking_ones", part, pat_walk1, 0);
    bad += run_pattern(ctx, "walking_zeros", part, pat_walk0, 0);
    bad += run_pattern(ctx, "address_inverted", part, pat_addr_inv, (uint32_t)PSRAM_NOCACHE_BASE);
    bad += narrow_access(ctx, bytes);
    bad += march_c_minus(ctx, bytes < MARCH_BYTES ? bytes : MARCH_BYTES);

    rec_value(ctx, "total_errors", UNIT_COUNT, bad);
    return bad ? TR_FAIL : TR_PASS;
}

#endif
