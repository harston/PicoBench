/*
 * SRAM integrity. Runs over the free SRAM (the unused heap), which spans every large
 * bank of the chip. The 4 KB scratch banks are not tested: they hold the stacks, and
 * testing them needs the stack moved first (left for a later phase).
 *
 * Patterns: constants, walking ones/zeros, address and inverted address (catches
 * addressing faults and aliasing), an LFSR sequence, and March C-, which catches
 * stuck-at, transition and coupling faults between cells.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/guard.h"
#include "pico/time.h"
#include "results/record.h"

typedef uint32_t (*pattern_fn)(uint32_t index, uint32_t seed);

static uint32_t pat_const(uint32_t i, uint32_t seed) { (void)i; return seed; }
static uint32_t pat_walk1(uint32_t i, uint32_t seed) { (void)seed; return 1u << (i & 31u); }
static uint32_t pat_walk0(uint32_t i, uint32_t seed) { (void)seed; return ~(1u << (i & 31u)); }
static uint32_t pat_addr(uint32_t i, uint32_t seed) { return seed + i * 4u; }
static uint32_t pat_addr_inv(uint32_t i, uint32_t seed) { return ~(seed + i * 4u); }

// Deterministic pseudo-random word for index i (no state, so the check pass can
// recompute it without a second buffer).
static uint32_t pat_lfsr(uint32_t i, uint32_t seed) {
    uint32_t x = seed ^ (i * 2654435761u);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

// Write the pattern, then read it back. Returns the number of mismatching words.
static uint32_t run_pattern(test_ctx_t *ctx, const char *name, uint8_t *ram, uint32_t bytes,
                            pattern_fn fn, uint32_t seed) {
    uint32_t *p = (uint32_t *)ram;
    uint32_t words = bytes / 4u;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < words; i++) p[i] = fn(i, seed);
    guard_keepalive();
    uint32_t bad = 0;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t want = fn(i, seed);
        uint32_t got = p[i];
        if (got != want) {
            bad++;
            rec_error(ctx, name, (uint32_t)(uintptr_t)&p[i], want, got);
        }
    }
    uint32_t us = (uint32_t)(time_us_64() - t0);
    char key[REC_LABEL_LEN + 8];
    snprintf(key, sizeof(key), "%s_us", name);
    rec_value(ctx, key, UNIT_US, us);
    guard_keepalive();
    return bad;
}

// March C-: {up (w0); up (r0,w1); up (r1,w0); down (r0,w1); down (r1,w0); down (r0)}
static uint32_t march_c_minus(test_ctx_t *ctx, uint8_t *ram, uint32_t bytes) {
    uint32_t *p = (uint32_t *)ram;
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

// Byte and halfword accesses: catches byte-lane faults that word patterns miss.
static uint32_t narrow_access(test_ctx_t *ctx, uint8_t *ram, uint32_t bytes) {
    uint32_t bad = 0;
    for (uint32_t i = 0; i < bytes; i++) ram[i] = (uint8_t)(i * 31u + 7u);
    for (uint32_t i = 0; i < bytes; i++) {
        uint8_t want = (uint8_t)(i * 31u + 7u);
        if (ram[i] != want) { bad++; rec_error(ctx, "byte_access", (uint32_t)(uintptr_t)&ram[i], want, ram[i]); }
    }
    guard_keepalive();
    uint16_t *h = (uint16_t *)ram;
    uint32_t hn = bytes / 2u;
    for (uint32_t i = 0; i < hn; i++) h[i] = (uint16_t)(i ^ 0xA5A5u);
    for (uint32_t i = 0; i < hn; i++) {
        uint16_t want = (uint16_t)(i ^ 0xA5A5u);
        if (h[i] != want) { bad++; rec_error(ctx, "halfword_access", (uint32_t)(uintptr_t)&h[i], want, h[i]); }
    }
    return bad;
}

test_result_t test_sram_integrity(test_ctx_t *ctx) {
    uint32_t total = 0;
    for (unsigned g = 0; g < (unsigned)BENCH_BANK_GROUPS; g++) {
        uint32_t size;
        uint8_t *ram = bench_group_ram(g, &size);
        size &= ~3u;
        if (size < 4096) continue;
        char key[REC_LABEL_LEN];
        snprintf(key, sizeof(key), "group%u", g);
        rec_text(ctx, key, "%s: 0x%08lx + %lu KB tested", bench_group_name(g),
                 (unsigned long)(uintptr_t)ram, (unsigned long)(size / 1024));

        total += run_pattern(ctx, "const_00", ram, size, pat_const, 0x00000000u);
        total += run_pattern(ctx, "const_ff", ram, size, pat_const, 0xFFFFFFFFu);
        total += run_pattern(ctx, "const_55", ram, size, pat_const, 0x55555555u);
        total += run_pattern(ctx, "const_aa", ram, size, pat_const, 0xAAAAAAAAu);
        total += run_pattern(ctx, "walking_ones", ram, size, pat_walk1, 0);
        total += run_pattern(ctx, "walking_zeros", ram, size, pat_walk0, 0);
        total += run_pattern(ctx, "address", ram, size, pat_addr, (uint32_t)(uintptr_t)ram);
        total += run_pattern(ctx, "address_inverted", ram, size, pat_addr_inv, (uint32_t)(uintptr_t)ram);
        total += run_pattern(ctx, "lfsr", ram, size, pat_lfsr, 0x1234ABCDu);
        total += narrow_access(ctx, ram, size);
        total += march_c_minus(ctx, ram, size);
    }
    rec_value(ctx, "total_errors", UNIT_COUNT, total);
    rec_text(ctx, "not_tested", "scratch banks (4 KB each) hold the stacks and are not tested here");
    return total ? TR_FAIL : TR_PASS;
}
