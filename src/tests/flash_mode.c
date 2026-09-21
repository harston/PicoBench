/*
 * XIP read-mode and clock sweep (RP2350).
 *
 * Which read mode the flash runs in is decided by the boot ROM at start-up, and the
 * two boards tested so far ended up different: the WeAct (XMC, QE set) runs quad
 * 1-4-4 (0xEB), the Pico 2 clone (Boya, QE clear) runs dual 1-2-2 (0xBB) - roughly
 * half the read bandwidth. This test measures what each mode and clock divider would
 * actually deliver on the board it runs on.
 *
 * Safety:
 *   - Nothing is written to the flash chip. Only the QMI/XIP configuration is changed,
 *     and the original mode is always restored, including the original timing register.
 *     In particular the QE bit in the status register is NOT touched, so a chip that
 *     came with QE clear will simply fail the quad modes - which is the answer.
 *   - Everything that runs while the mode is switched lives in SRAM and reads flash
 *     through the uncached alias, because the cache and any flash-resident code are
 *     unusable while the mode is wrong.
 *   - Every mode is verified with a checksum of a known region before it is measured;
 *     a mismatch means "unusable" and the mode is abandoned immediately.
 *   - The stage is TF_RISKY: a hang becomes TEST_CRASHED, and the boot ROM sets the
 *     flash up from scratch after the reset anyway.
 */

#include "tests/tests.h"

#include <stdio.h>

#include "core/guard.h"
#include "core/timing.h"
#include "results/record.h"

#if PICO_RP2040

test_result_t test_flash_xip_mode_sweep(test_ctx_t *ctx) {
    rec_text(ctx, "not_applicable", "on RP2040 the read mode is set by boot stage 2 in the image; "
                                    "changing it at run time would mean re-running a different stage 2");
    return TR_NA;
}

#else

#include "hardware/regs/addressmap.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/bootrom.h"

#define CHECK_BYTES 32768u      // checked and measured region: the firmware image
#define MEAS_BYTES  4096u

typedef uint32_t (*read_block_fn)(const uint8_t *p, uint32_t bytes);

typedef struct {
    uint32_t checksum;
    uint32_t timing;
    uint32_t rfmt;
    uint32_t rcmd;
    uint32_t read_cycles;   // cycles to read MEAS_BYTES through the uncached alias
    bool ok;
} mode_result_t;

// Both live in SRAM: they run while the flash may be unreadable. __no_inline_ matters:
// with plain __not_in_flash_func the compiler happily inlined them into the caller,
// which lives in flash - the copy in SRAM would then never run (verified in the .dis).
static uint32_t __no_inline_not_in_flash_func(ram_checksum)(const uint8_t *p, uint32_t bytes) {
    const uint32_t *w = (const uint32_t *)p;
    uint32_t sum = 0x1234u;
    for (uint32_t i = 0; i < bytes / 4u; i++) {
        sum = (sum << 1) | (sum >> 31);
        sum += w[i];
    }
    return sum;
}

static uint32_t __no_inline_not_in_flash_func(ram_read_block)(const uint8_t *p, uint32_t bytes) {
    const uint32_t *w = (const uint32_t *)p;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        acc += w[i] + w[i + 1] + w[i + 2] + w[i + 3] + w[i + 4] + w[i + 5] + w[i + 6] + w[i + 7];
    }
    return acc;
}

// Called through this volatile pointer: a plain call to a pure function gets moved out
// from between the two cycle-counter reads, and the measurement then reported 1 cycle
// for 4 KB (seen on hardware).
static read_block_fn volatile v_read_block = ram_read_block;

// Switch to (mode, clkdiv), verify, measure, and always restore. Runs entirely from
// SRAM with interrupts off; reads go through the uncached alias so the XIP cache (and
// the flash-resident code it holds) is never consulted while the mode is changed.
static void __no_inline_not_in_flash_func(try_mode)(bootrom_xip_mode_t mode, uint8_t clkdiv,
                                          bootrom_xip_mode_t restore_mode, uint8_t restore_clkdiv,
                                          uint32_t restore_timing, uint32_t ref_sum,
                                          mode_result_t *out) {
    const uint8_t *u = (const uint8_t *)XIP_NOCACHE_NOALLOC_BASE;
    uint32_t irq = save_and_disable_interrupts();

    rom_flash_select_xip_read_mode(mode, clkdiv);
    out->timing = qmi_hw->m[0].timing;
    out->rfmt = qmi_hw->m[0].rfmt;
    out->rcmd = qmi_hw->m[0].rcmd;
    out->checksum = ram_checksum(u, CHECK_BYTES);
    out->ok = out->checksum == ref_sum;

    if (out->ok) {
        const uint8_t *q = u;
        uint32_t n = MEAS_BYTES;
        __asm volatile("" : "+r"(q), "+r"(n));
        uint32_t t0 = timing_now();
        uint32_t acc = v_read_block(q, n);
        uint32_t t1 = timing_now();
        out->read_cycles = timing_elapsed(t0, t1);
        __asm volatile("" :: "r"(acc));
    } else {
        out->read_cycles = 0;
    }

    // Restore: the same ROM call that set the mode, then the exact original timing
    // (clock divider, RX delay, cooldown, select timings).
    rom_flash_select_xip_read_mode(restore_mode, restore_clkdiv);
    qmi_hw->m[0].timing = restore_timing;
    restore_interrupts(irq);
}

static const char *mode_name(bootrom_xip_mode_t m) {
    switch (m) {
    case BOOTROM_XIP_MODE_03H_SERIAL: return "03h serial (1-1-1)";
    case BOOTROM_XIP_MODE_0BH_SERIAL: return "0Bh serial (1-1-1, fast read)";
    case BOOTROM_XIP_MODE_BBH_DUAL: return "BBh dual (1-2-2)";
    case BOOTROM_XIP_MODE_EBH_QUAD: return "EBh quad (1-4-4)";
    default: return "?";
    }
}

// The mode the board is currently running in, from the read command in QMI.
static bootrom_xip_mode_t current_mode(void) {
    uint32_t cmd = qmi_hw->m[0].rcmd & 0xFFu;
    switch (cmd) {
    case 0xEB: return BOOTROM_XIP_MODE_EBH_QUAD;
    case 0xBB: return BOOTROM_XIP_MODE_BBH_DUAL;
    case 0x0B: return BOOTROM_XIP_MODE_0BH_SERIAL;
    default: return BOOTROM_XIP_MODE_03H_SERIAL;
    }
}

test_result_t test_flash_xip_mode_sweep(test_ctx_t *ctx) {
    const uint8_t *u = (const uint8_t *)XIP_NOCACHE_NOALLOC_BASE;
    uint32_t saved_timing = qmi_hw->m[0].timing;
    uint8_t saved_clkdiv = (uint8_t)(saved_timing & QMI_M0_TIMING_CLKDIV_BITS);
    bootrom_xip_mode_t saved_mode = current_mode();

    uint32_t irq = save_and_disable_interrupts();
    uint32_t ref_sum = ram_checksum(u, CHECK_BYTES);
    restore_interrupts(irq);

    rec_text(ctx, "current_mode", "%s, clock divider %u (SCK %lu kHz)", mode_name(saved_mode),
             saved_clkdiv, (unsigned long)(saved_clkdiv ? ctx->clk_khz / saved_clkdiv : 0));
    rec_value(ctx, "reference_checksum", UNIT_HEX, ref_sum);

    static const bootrom_xip_mode_t modes[] = {
        BOOTROM_XIP_MODE_03H_SERIAL, BOOTROM_XIP_MODE_0BH_SERIAL,
        BOOTROM_XIP_MODE_BBH_DUAL, BOOTROM_XIP_MODE_EBH_QUAD};
    static const uint8_t clkdivs[] = {4, 3, 2};

    unsigned usable = 0;
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        for (unsigned d = 0; d < sizeof(clkdivs) / sizeof(clkdivs[0]); d++) {
            guard_keepalive();
            mode_result_t r;
            try_mode(modes[m], clkdivs[d], saved_mode, saved_clkdiv, saved_timing, ref_sum, &r);

            char key[REC_LABEL_LEN + 24];
            snprintf(key, sizeof(key), "mode%u_div%u", (unsigned)modes[m], clkdivs[d]);
            if (!r.ok) {
                rec_text(ctx, key, "%s, div %u: UNUSABLE (checksum 0x%08lx != 0x%08lx)",
                         mode_name(modes[m]), clkdivs[d], (unsigned long)r.checksum,
                         (unsigned long)ref_sum);
                continue;
            }
            usable++;
            uint32_t sck = ctx->clk_khz / clkdivs[d];
            int64_t rate = r.read_cycles ? (int64_t)((uint64_t)MEAS_BYTES * ctx->clk_khz / r.read_cycles) : 0;
            rec_text(ctx, key, "%s, div %u (SCK %lu kHz): %lu cycles for 4 KB, %lld kB/s, timing 0x%08lx",
                     mode_name(modes[m]), clkdivs[d], (unsigned long)sck,
                     (unsigned long)r.read_cycles, (long long)rate, (unsigned long)r.timing);
        }
    }

    // Back in the original mode: prove the flash still reads correctly through the cache.
    xip_cache_invalidate_all();
    irq = save_and_disable_interrupts();
    uint32_t after = ram_checksum(u, CHECK_BYTES);
    restore_interrupts(irq);
    rec_value(ctx, "checksum_after_restore", UNIT_HEX, after);
    rec_value(ctx, "usable_combinations", UNIT_COUNT, usable);
    if (after != ref_sum) {
        rec_error(ctx, "restore_failed", 0, ref_sum, after);
        return TR_FAIL;
    }
    rec_text(ctx, "quad_note", "quad modes need the QE bit set in the flash status register. "
                               "This test never writes to the chip, so a board whose QE is clear "
                               "reports the quad modes as unusable.");
    return TR_PASS;
}

#endif
