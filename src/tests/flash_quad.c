/*
 * Quad Enable (QE) check and quad-read trial (RP2350).
 *
 * The boot ROM reads the flash in quad 1-4-4 only when the chip's QE bit is set. The
 * RP2350A clone's Boya chip ships with QE clear and runs dual 1-2-2, ~1.5x slower than
 * the WeAct's quad. This test answers whether that is only a setting:
 *
 *   1. Gate: it runs only if every flash and PSRAM test so far - all of them at the
 *      board's default settings - passed. It is the last stage at the default
 *      operating point (the end of g_tests[]), so they have all run by then.
 *   2. QE clear: it tries to set it in the VOLATILE status register: 50h ("write enable
 *      for volatile status register") then 31h (write SR2), and if the chip ignores
 *      that, 50h then 01h (write SR1+SR2, SR1 unchanged). A volatile write changes
 *      nothing permanently - the chip forgets it at power-off - and the test clears it
 *      again at the end. A non-volatile write (06h) is never attempted. If the chip does
 *      not take the volatile write, that is the result and the test ends.
 *   3. QE set (by the board, or by step 2): quad 1-4-4 is tried at clock dividers
 *      4/3/2. Each setting is verified first: a 128 KB region is read PASSES times and
 *      every 4 KB block compared with its checksum taken in the default mode. Bad
 *      blocks are the result for that divider ("UNSTABLE"), not a test failure.
 *      A clean setting is measured: uncached 4 KB read and cold cache-miss latency,
 *      next to the same two figures in the board's default mode.
 *   4. Read mode, timing register, QE and the PSRAM window are restored, and the whole
 *      region is verified again through the default mode. Only a failed restore, a
 *      changed SR1 or a bad default-mode read is a test failure (rec_error).
 *
 * flash.continuous_read (432) reuses the same gate, QE handling and verification to try
 * continuous read mode (no command byte per access) in quad and dual; see the comment
 * above test_flash_continuous_read(). flash.sck_limit (433) uses them to find the
 * fastest flash clock at every operating point; see test_flash_sck_limit().
 *
 * Everything that runs while the read mode is switched lives in SRAM with interrupts
 * off (same rules as flash_mode.c). Residual risk: a reset in the middle of step 3
 * (watchdog -> TEST_CRASHED) leaves a QE bit set here in place until power-off, and
 * the boot ROM may then pick quad on the next boot. HARDWARE.TXT of such a boot shows
 * SR2 QE set; a power cycle clears it. A reset during a 432 trial can also leave the chip
 * in continuous read mode; the boot ROM's own XIP exit sequence should take it out, and
 * a power cycle certainly does.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "detect/psram_info.h"
#include "results/record.h"
#include "results/summary.h"
#include "tests/flash_fast.h"

// flash.sck_limit (433): the fastest SCK tried. 104 MHz is the lowest maximum among the
// 3 V QSPI NOR families on these boards (W25Q128FV class; newer parts are rated 133
// MHz). The overclocking variant raises it to see where the reads actually fail.
#ifndef PICOBENCH_FLASH_SCK_LIMIT_KHZ
#define PICOBENCH_FLASH_SCK_LIMIT_KHZ 104000u
#endif
#define SCAN_PASSES 4u                // reads of the verified region per scanned setting

// Longest run of set bits in mask (bits 0..7): first bit and length.
static unsigned longest_run(uint32_t mask, unsigned *first) {
    unsigned best = 0, best_first = 0;
    for (unsigned i = 0; i < 8; i++) {
        unsigned n = 0;
        while (i + n < 8 && (mask & (1u << (i + n)))) n++;
        if (n > best) { best = n; best_first = i; }
    }
    *first = best_first;
    return best;
}

// "1-3" style list of the set bits (0..max).
static void mask_text(char *out, size_t len, uint32_t mask, unsigned max) {
    size_t n = 0;
    out[0] = '\0';
    for (unsigned i = 0; i <= max && n < len; i++) {
        if (!(mask & (1u << i))) continue;
        unsigned j = i;
        while (j + 1 <= max && (mask & (1u << (j + 1)))) j++;
        n += (size_t)snprintf(out + n, len - n, "%s%u", n ? "," : "", i);
        if (j > i && n < len) n += (size_t)snprintf(out + n, len - n, "-%u", j);
        i = j;
    }
    if (!mask) snprintf(out, len, "none");
}

#if PICO_RP2040

test_result_t test_flash_quad_enable(test_ctx_t *ctx) {
    rec_text(ctx, "not_applicable", "on RP2040 boot stage 2 sets QE and selects the read mode itself; "
                                    "it cannot be changed at run time");
    return TR_NA;
}

test_result_t test_flash_continuous_read(test_ctx_t *ctx) {
    rec_text(ctx, "not_applicable", "on RP2040 boot stage 2 already runs the flash in quad continuous "
                                    "read (no command byte per access)");
    return TR_NA;
}

// RP2040 (433): boot stage 2 runs quad continuous read at SSI divider 2 (even dividers
// only, 2 is the smallest), so what is left to find out is the RX sample delay window at
// dividers 2 and 4 at this operating point - how much timing margin the setting in use
// has. Each setting is verified by reading 64 KB back 4 times; the setting this
// operating point uses is put back afterwards and checked again.
#include "core/flash_clock.h"

#define RP2040_SCAN_BYTES (64u * 1024u)
#define RP2040_RX_MAX 4u

test_result_t test_flash_sck_limit(test_ctx_t *ctx) {
    flash_clock_t op_fc;
    flash_clock_get(&op_fc);
    const uint32_t ref = flash_clock_checksum(RP2040_SCAN_BYTES);
    rec_text(ctx, "in_use", "SSI divider %u (SCK %lu kHz), RX sample delay %u", op_fc.div,
             (unsigned long)(ctx->clk_khz / op_fc.div), op_fc.rxdelay);
    static const uint16_t divs[] = {2, 4};
    for (unsigned d = 0; d < sizeof divs / sizeof divs[0]; d++) {
        char key[REC_LABEL_LEN], list[32];
        uint32_t sck = ctx->clk_khz / divs[d];
        snprintf(key, sizeof key, "div%u", divs[d]);
        if (sck > PICOBENCH_FLASH_SCK_LIMIT_KHZ) {
            rec_text(ctx, key, "SCK %lu kHz: not tried, above the %lu kHz limit", (unsigned long)sck,
                     (unsigned long)PICOBENCH_FLASH_SCK_LIMIT_KHZ);
            continue;
        }
        uint32_t mask = 0;
        for (unsigned rx = 0; rx <= RP2040_RX_MAX; rx++) {
            guard_keepalive();
            flash_clock_t fc = {divs[d], (uint8_t)rx, 0};
            if (flash_clock_try(&fc, ref, RP2040_SCAN_BYTES, SCAN_PASSES)) {
                mask |= 1u << rx;
                flash_clock_set(&op_fc);
            }
        }
        mask_text(list, sizeof list, mask, RP2040_RX_MAX);
        rec_text(ctx, key, "SCK %lu kHz: RX sample delay %s of 0-%u read correctly (%u x %lu KB each)",
                 (unsigned long)sck, list, RP2040_RX_MAX, SCAN_PASSES,
                 (unsigned long)(RP2040_SCAN_BYTES / 1024u));
        snprintf(key, sizeof key, "div%u_rx_ok_mask", divs[d]);
        rec_value(ctx, key, UNIT_HEX, mask);
    }
    flash_clock_set(&op_fc);
    uint32_t after = flash_clock_checksum(RP2040_SCAN_BYTES);
    if (after != ref) {
        rec_error(ctx, "restore_failed", 0, ref, after);
        return TR_FAIL;
    }
    rec_text(ctx, "note", "only the RP2040 side changes (SSI divider and sample delay); the flash chip "
                          "stays in boot stage 2's quad continuous read mode");
    return TR_PASS;
}

// ---- flash_fast.h (overclocking explorer) -------------------------------------------

#define FAST_MISS_SAMPLES 64u
static uint32_t s_fast_ref;
static bool s_fast_ready;

void flash_fast_prepare(void) {
    s_fast_ref = flash_clock_checksum(RP2040_SCAN_BYTES);
    s_fast_ready = true;
}

void flash_fast_probe(uint32_t clk_khz, uint32_t sck_max_khz, flash_fast_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s_fast_ready) {
        out->why = "not prepared";
        return;
    }
    flash_clock_t cur;
    flash_clock_get(&cur);
    out->tried = true;
    out->div = flash_clock_div_for(clk_khz, sck_max_khz);
    out->sck_khz = clk_khz / out->div;
    for (unsigned rx = 0; rx < 8; rx++) {
        flash_clock_t fc = {out->div, (uint8_t)rx, 0};
        if (flash_clock_try(&fc, s_fast_ref, RP2040_SCAN_BYTES, 2)) {
            out->rx_mask |= (uint8_t)(1u << rx);
            flash_clock_set(&cur);
        }
    }
    if (!out->rx_mask) return;
    unsigned first, len = longest_run(out->rx_mask, &first);
    out->rx = (uint8_t)(first + (len - 1u) / 2u);
    flash_clock_t fc = {out->div, out->rx, 0};
    if (flash_clock_try(&fc, s_fast_ref, RP2040_SCAN_BYTES, SCAN_PASSES)) {
        out->verified = true;
        stats_t st;
        bench_cold_reads((const uint8_t *)(XIP_BASE + 32u * 1024u), 64u, FAST_MISS_SAMPLES, g_bench_samples);
        stats_from_samples(g_bench_samples, FAST_MISS_SAMPLES, &st);
        out->miss_p50 = st.p50;
        out->miss_max = st.max;
        flash_clock_set(&cur);
    }
}

#else

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/structs/watchdog.h"
#include "hardware/xip_cache.h"
#include "pico/bootrom.h"

#define VERIFY_BYTES (128u * 1024u)   // start of the firmware image: varied data
#define BLOCK_BYTES  4096u
#define NBLOCKS      (VERIFY_BYTES / BLOCK_BYTES)
#define PASSES       16u
#define MEAS_BYTES   4096u
#define MISS_SAMPLES 256u
#define MISS_STRIDE  64u
#define QE_BIT       0x02u            // SR2 bit 1 (Winbond layout; Boya and XMC use it too)
#define NO_BLOCK     0xFFFFFFFFu
#define MODE_CONTINUOUS 0xA0u         // mode byte: M5-4 = 10 -> next read has no command byte
#define RXDELAY_ROM  0xFFu            // run_continuous(): keep the ROM's RX delay

typedef uint32_t (*read_block_fn)(const uint8_t *p, uint32_t bytes);

typedef struct {
    uint32_t timing;          // QMI M0 timing register while the mode was active
    uint32_t bad_blocks;      // 4 KB blocks that read wrong, over all passes
    uint32_t first_bad;       // index of the first bad block, NO_BLOCK if none
    uint32_t read_cycles;     // MEAS_BYTES through the uncached alias
    bool measured;
} mode_result_t;

typedef struct {
    bootrom_xip_mode_t mode;  // the board's default read mode, as the boot ROM left it
    uint8_t div;
    uint32_t timing, rfmt, rcmd;
} saved_qmi_t;

typedef struct {
    uint32_t rom_rfmt, rom_rcmd;  // what the ROM sets up for this mode (with the command byte)
    uint32_t probe;               // first word read without the command byte
    bool has_mode_byte;           // the ROM's format carries a mode byte that can hold A0h
    bool engaged;                 // the chip answered the probe correctly
    mode_result_t r;
} cont_result_t;

static uint32_t s_ref[NBLOCKS];       // block checksums in the default mode
static volatile uint32_t s_sink;

// ---- SRAM-resident code (runs while the flash read mode may be wrong) --------------

static uint32_t __no_inline_not_in_flash_func(block_sum)(const uint8_t *p) {
    const uint32_t *w = (const uint32_t *)p;
    uint32_t sum = 0x1234u;
    for (uint32_t i = 0; i < BLOCK_BYTES / 4u; i++) {
        sum = (sum << 1) | (sum >> 31);
        sum += w[i];
    }
    return sum;
}

static uint32_t __no_inline_not_in_flash_func(read_block)(const uint8_t *p, uint32_t bytes) {
    const uint32_t *w = (const uint32_t *)p;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4u; i += 8) {
        acc += w[i] + w[i + 1] + w[i + 2] + w[i + 3] + w[i + 4] + w[i + 5] + w[i + 6] + w[i + 7];
    }
    return acc;
}

// Through a volatile pointer: a direct call to a pure function gets moved out of the
// timed region (flash_mode.c reported 1 cycle for 4 KB before the same fix).
static read_block_fn volatile v_read_block = read_block;

// Verify (`passes` x every block) in whatever mode QMI is in now, then measure if clean
// and `measure` is set. A scan (measure = false) stops at the first bad block.
// Interrupts must already be off.
static void __no_inline_not_in_flash_func(verify_and_measure)(uint32_t *miss_cycles, uint32_t passes,
                                                              bool measure, mode_result_t *out) {
    const uint8_t *u = (const uint8_t *)XIP_NOCACHE_NOALLOC_BASE;
    out->timing = qmi_hw->m[0].timing;
    out->bad_blocks = 0;
    out->first_bad = NO_BLOCK;
    out->read_cycles = 0;
    out->measured = false;

    for (uint32_t pass = 0; pass < passes; pass++) {
        for (uint32_t b = 0; b < NBLOCKS; b++) {
            if (block_sum(u + b * BLOCK_BYTES) != s_ref[b]) {
                out->bad_blocks++;
                if (out->first_bad == NO_BLOCK) out->first_bad = b;
                if (!measure) return;
            }
        }
    }

    if (out->bad_blocks == 0 && measure) {
        const uint8_t *q = u;
        uint32_t n = MEAS_BYTES;
        __asm volatile("" : "+r"(q), "+r"(n));
        uint32_t t0 = timing_now();
        uint32_t acc = v_read_block(q, n);
        uint32_t t1 = timing_now();
        out->read_cycles = timing_elapsed(t0, t1);
        s_sink += acc;

        // Cold cache miss: invalidate, then one word from a line not read before.
        for (uint32_t i = 0; i < MISS_SAMPLES; i++) {
            xip_cache_invalidate_all();
            const volatile uint32_t *p =
                (const volatile uint32_t *)(XIP_BASE + (i * MISS_STRIDE) % VERIFY_BYTES);
            __asm volatile("" : "+r"(p));
            uint32_t m0 = timing_now();
            uint32_t v = *p;
            uint32_t m1 = timing_now();
            miss_cycles[i] = timing_net(m0, m1);
            s_sink += v;
        }
        out->measured = true;
    }
}

// switch_mode = false measures the board's current (default) mode. Always restores mode
// and timing before returning.
static void __no_inline_not_in_flash_func(run_mode)(bool switch_mode, bootrom_xip_mode_t mode,
                                                    uint8_t clkdiv, bootrom_xip_mode_t restore_mode,
                                                    uint8_t restore_clkdiv, uint32_t restore_timing,
                                                    uint32_t *miss_cycles, mode_result_t *out) {
    uint32_t irq = save_and_disable_interrupts();
    if (switch_mode) rom_flash_select_xip_read_mode(mode, clkdiv);
    verify_and_measure(miss_cycles, PASSES, true, out);
    if (switch_mode) {
        rom_flash_select_xip_read_mode(restore_mode, restore_clkdiv);
        qmi_hw->m[0].timing = restore_timing;
    }
    // The cache now holds lines read in the trial mode; drop them.
    xip_cache_invalidate_all();
    restore_interrupts(irq);
}

static void __no_inline_not_in_flash_func(take_reference)(void) {
    const uint8_t *u = (const uint8_t *)XIP_NOCACHE_NOALLOC_BASE;
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t b = 0; b < NBLOCKS; b++) s_ref[b] = block_sum(u + b * BLOCK_BYTES);
    restore_interrupts(irq);
}

// Puts QMI and the chip back into the board's default mode: the ROM's exit sequence
// takes the chip out of continuous read, the ROM sets the default mode up again, and the
// exact original registers are written back. Interrupts must already be off.
static void __no_inline_not_in_flash_func(restore_default)(const saved_qmi_t *sv) {
    rom_connect_internal_flash();
    rom_flash_exit_xip();
    rom_flash_flush_cache();
    rom_flash_select_xip_read_mode(sv->mode, sv->div);
    qmi_hw->m[0].timing = sv->timing;
    qmi_hw->m[0].rfmt = sv->rfmt;
    qmi_hw->m[0].rcmd = sv->rcmd;
    xip_cache_invalidate_all();
}

// One continuous-read trial, entirely in SRAM with interrupts off. Same sequence as the
// SDK's boot2_w25q080: one read with the command byte and mode byte A0h, then reads
// without the command byte.
// Safety: if the chip did NOT take the mode bits, it reads the address bits on IO0 as an
// opcode. With mode byte A0h the two lowest opcode bits are 0, so WREN (06h), status or
// page writes and 0xC7 can never form, and erases need WREN. Still, only address 0 -
// opcode 00h, a no-op - is read until the chip has answered correctly there.
// If the default mode does not read back afterwards, all code in flash is unreachable:
// the watchdog resets the chip (TEST_CRASHED) instead of returning into flash.
// rxdelay = RXDELAY_ROM keeps the ROM's sample delay; anything else replaces it (433).
static void __no_inline_not_in_flash_func(run_continuous)(bootrom_xip_mode_t mode, uint8_t clkdiv,
                                                          uint8_t rxdelay, const saved_qmi_t *sv,
                                                          uint32_t probe_ref, uint32_t passes, bool measure,
                                                          uint32_t *miss_cycles, cont_result_t *out) {
    const volatile uint32_t *u = (const volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    uint32_t irq = save_and_disable_interrupts();
    rom_flash_select_xip_read_mode(mode, clkdiv);
    if (rxdelay != RXDELAY_ROM) {
        qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~QMI_M0_TIMING_RXDELAY_BITS) |
                              ((uint32_t)rxdelay << QMI_M0_TIMING_RXDELAY_LSB);
    }
    uint32_t rfmt = qmi_hw->m[0].rfmt, rcmd = qmi_hw->m[0].rcmd;
    out->rom_rfmt = rfmt;
    out->rom_rcmd = rcmd;
    out->has_mode_byte = (rfmt & QMI_M0_RFMT_SUFFIX_LEN_BITS) != 0;
    out->engaged = false;
    out->probe = 0;
    out->r.measured = false;
    out->r.bad_blocks = 0;
    out->r.first_bad = NO_BLOCK;
    if (out->has_mode_byte) {
        qmi_hw->m[0].rcmd = (rcmd & ~QMI_M0_RCMD_SUFFIX_BITS) | (MODE_CONTINUOUS << QMI_M0_RCMD_SUFFIX_LSB);
        (void)u[0];                                             // command + A0h: chip enters it
        qmi_hw->m[0].rfmt = rfmt & ~QMI_M0_RFMT_PREFIX_LEN_BITS; // no command byte from here on
        out->probe = u[0];
        out->engaged = out->probe == probe_ref;
        if (out->engaged) verify_and_measure(miss_cycles, passes, measure, &out->r);
    }
    restore_default(sv);
    if (block_sum((const uint8_t *)u) != s_ref[0]) {
        restore_default(sv);
        if (block_sum((const uint8_t *)u) != s_ref[0]) {
            hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
            while (true) {}
        }
    }
    restore_interrupts(irq);
}

static uint32_t __no_inline_not_in_flash_func(read_word0)(void) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t v = *(const volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    restore_interrupts(irq);
    return v;
}

// ---- status register access (flash_do_cmd saves and restores the QMI setup) --------

static void flash_cmd(const uint8_t *tx, uint8_t *rx, size_t n) {
    uint32_t irq = save_and_disable_interrupts();
    flash_do_cmd(tx, rx, n);
    restore_interrupts(irq);
}

static uint8_t read_sr(uint8_t cmd) {
    uint8_t tx[2] = {cmd, 0}, rx[2] = {0};
    flash_cmd(tx, rx, 2);
    return rx[1];
}

static bool wait_not_busy(void) {
    for (unsigned i = 0; i < 1000; i++) {
        if (!(read_sr(0x05) & 0x01u)) return true;
    }
    return false;
}

typedef enum { WRSR_SR2_31H = 0, WRSR_SR1_SR2_01H, WRSR_METHODS } wrsr_method_t;

static const char *method_name(wrsr_method_t m) {
    return m == WRSR_SR2_31H ? "50h + 31h (SR2, volatile)" : "50h + 01h (SR1+SR2, volatile)";
}

// Volatile status-register write. Never 06h: nothing here may outlive a power cycle.
static bool write_sr_volatile(wrsr_method_t m, uint8_t sr1, uint8_t sr2) {
    uint8_t wen = 0x50, dummy;
    flash_cmd(&wen, &dummy, 1);
    uint8_t tx[3], rx[3];
    size_t n;
    if (m == WRSR_SR2_31H) {
        tx[0] = 0x31; tx[1] = sr2; n = 2;
    } else {
        tx[0] = 0x01; tx[1] = sr1; tx[2] = sr2; n = 3;
    }
    flash_cmd(tx, rx, n);
    return wait_not_busy();
}

// ---- helpers -------------------------------------------------------------------------

static bootrom_xip_mode_t current_mode(void) {
    switch (qmi_hw->m[0].rcmd & 0xFFu) {
    case 0xEB: return BOOTROM_XIP_MODE_EBH_QUAD;
    case 0xBB: return BOOTROM_XIP_MODE_BBH_DUAL;
    case 0x0B: return BOOTROM_XIP_MODE_0BH_SERIAL;
    default: return BOOTROM_XIP_MODE_03H_SERIAL;
    }
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

// Records one measured mode under "<prefix>_read" and "<prefix>_miss_cold".
static void record_mode(test_ctx_t *ctx, const char *prefix, const char *desc, uint8_t clkdiv,
                        const mode_result_t *r) {
    char key[REC_LABEL_LEN];
    int64_t rate = r->read_cycles ? (int64_t)((uint64_t)MEAS_BYTES * ctx->clk_khz / r->read_cycles) : 0;
    snprintf(key, sizeof key, "%s", prefix);
    rec_text(ctx, key, "%s, div %u (SCK %lu kHz), timing 0x%08lx: %u x 128 KB read without error",
             desc, clkdiv, (unsigned long)(clkdiv ? ctx->clk_khz / clkdiv : 0), (unsigned long)r->timing,
             PASSES);
    snprintf(key, sizeof key, "%s_read4k", prefix);
    rec_value(ctx, key, UNIT_CYCLES, r->read_cycles);
    snprintf(key, sizeof key, "%s_rate", prefix);
    rec_value(ctx, key, UNIT_KBPS, rate);
    stats_t s;
    stats_from_samples(g_bench_samples, MISS_SAMPLES, &s);
    snprintf(key, sizeof key, "%s_miss_cold", prefix);
    rec_stats(ctx, key, UNIT_CYCLES, 1, &s);
}

// ---- gate and QE handling, shared by 431 and 432 ---------------------------------

// Runs only if every flash and PSRAM test so far - all at the board's default settings -
// passed. Records the decision either way.
static bool gate_ok(test_ctx_t *ctx) {
    run_summary_t sum;
    summary_compute(&sum);
    const subsys_summary_t *f = &sum.sub[SUBSYS_FLASH], *p = &sum.sub[SUBSYS_PSRAM];
    if (!f->pass || f->fail || f->crashed || f->skipped || f->errors || p->fail || p->crashed ||
        p->skipped || p->errors) {
        rec_text(ctx, "gate", "not run: the flash/PSRAM tests at default settings did not all pass "
                              "(flash pass %u fail %u crashed %u skipped %u; PSRAM fail %u crashed %u)",
                 f->pass, f->fail, f->crashed, f->skipped, p->fail, p->crashed);
        return false;
    }
    rec_text(ctx, "gate", "flash tests at default settings: %u passed, none failed; PSRAM: none failed",
             f->pass);
    return true;
}

typedef enum { QE_ALREADY, QE_SET_HERE, QE_NOT_POSSIBLE, QE_ERROR } qe_outcome_t;

typedef struct {
    uint8_t sr1, sr2;         // status registers before the test
    wrsr_method_t method;     // how QE was set, if it was
    qe_outcome_t outcome;
} qe_state_t;

// QE already set, or set it in the volatile status register. Records what happened.
static qe_outcome_t qe_ensure(test_ctx_t *ctx, qe_state_t *st) {
    st->sr1 = read_sr(0x05);
    st->sr2 = read_sr(0x35);
    st->method = WRSR_SR2_31H;
    rec_text(ctx, "status_before", "SR1 %02x SR2 %02x: QE %s", st->sr1, st->sr2,
             (st->sr2 & QE_BIT) ? "set" : "clear");
    if (st->sr2 & QE_BIT) {
        rec_text(ctx, "qe_enable", "already set on this board (non-volatile); nothing written");
        return st->outcome = QE_ALREADY;
    }
    uint8_t now1 = st->sr1, now2 = st->sr2;
    for (unsigned m = 0; m < WRSR_METHODS; m++) {
        st->method = (wrsr_method_t)m;
        if (!write_sr_volatile(st->method, st->sr1, (uint8_t)(st->sr2 | QE_BIT))) {
            rec_text(ctx, "qe_enable", "%s: flash stayed busy", method_name(st->method));
            rec_error(ctx, "wrsr_busy", 0, 0, 1);
            return st->outcome = QE_ERROR;
        }
        now1 = read_sr(0x05);
        now2 = read_sr(0x35);
        if (now1 != st->sr1) {
            // Must never happen: put SR1 back (volatile) and stop.
            write_sr_volatile(WRSR_SR1_SR2_01H, st->sr1, st->sr2);
            rec_error(ctx, "sr1_changed", 0, st->sr1, now1);
            return st->outcome = QE_ERROR;
        }
        if (now2 & QE_BIT) {
            rec_text(ctx, "qe_enable", "set by this test with %s; SR2 now %02x. The chip forgets it at "
                                       "power-off and the test clears it at the end.",
                     method_name(st->method), now2);
            return st->outcome = QE_SET_HERE;
        }
    }
    rec_text(ctx, "qe_enable", "NOT POSSIBLE: the chip ignored both volatile status-register "
                               "writes (SR2 still %02x). A non-volatile write was not attempted: "
                               "it would change the chip permanently.", now2);
    return st->outcome = QE_NOT_POSSIBLE;
}

// Clears QE again if qe_ensure() set it. Returns false (and records an error) on failure.
static bool qe_restore(test_ctx_t *ctx, const qe_state_t *st) {
    if (st->outcome != QE_SET_HERE) return true;
    bool ok = write_sr_volatile(st->method, st->sr1, st->sr2);
    uint8_t now2 = read_sr(0x35);
    ok = ok && now2 == st->sr2;
    rec_text(ctx, "qe_restore", "%s (SR2 %02x)", ok ? "QE cleared again" : "RESTORE FAILED", now2);
    if (!ok) rec_error(ctx, "qe_restore", 0, st->sr2, now2);
    return ok;
}

static void save_qmi(saved_qmi_t *sv) {
    sv->timing = qmi_hw->m[0].timing;
    sv->rfmt = qmi_hw->m[0].rfmt;
    sv->rcmd = qmi_hw->m[0].rcmd;
    sv->div = (uint8_t)(sv->timing & QMI_M0_TIMING_CLKDIV_BITS);
    sv->mode = current_mode();
}

// ---- the tests -----------------------------------------------------------------------

test_result_t test_flash_quad_enable(test_ctx_t *ctx) {
    // 1. Gate: everything at the default settings must have passed.
    if (!gate_ok(ctx)) return TR_SKIPPED;

    const uint32_t saved_timing = qmi_hw->m[0].timing;
    const uint8_t saved_div = (uint8_t)(saved_timing & QMI_M0_TIMING_CLKDIV_BITS);
    const bootrom_xip_mode_t saved_mode = current_mode();
    rec_text(ctx, "default_mode", "%s, clock divider %u (SCK %lu kHz)", mode_name(saved_mode), saved_div,
             (unsigned long)(saved_div ? ctx->clk_khz / saved_div : 0));

    // 2. QE: already set, or can it be set (volatile)?
    qe_state_t qe;
    qe_outcome_t qo = qe_ensure(ctx, &qe);
    if (qo == QE_ERROR) return TR_FAIL;
    if (qo == QE_NOT_POSSIBLE) {
        rec_value(ctx, "qe_enable_possible", UNIT_BOOL, 0);
        return TR_PASS;
    }
    rec_value(ctx, "qe_enable_possible", UNIT_BOOL, 1);

    // 3. Reference and default mode, then quad at dividers 4/3/2.
    xip_cache_clean_all();      // write back any cached PSRAM data before invalidating
    take_reference();
    mode_result_t r;
    run_mode(false, saved_mode, saved_div, saved_mode, saved_div, saved_timing, g_bench_samples, &r);
    bool default_ok = r.measured;
    if (default_ok) {
        record_mode(ctx, "default", mode_name(saved_mode), saved_div, &r);
    } else {
        rec_error(ctx, "default_mode_read", r.first_bad, 0, r.bad_blocks);
    }

    static const uint8_t clkdivs[] = {4, 3, 2};
    unsigned clean = 0;
    for (unsigned d = 0; d < sizeof clkdivs && default_ok; d++) {
        guard_keepalive();
        run_mode(true, BOOTROM_XIP_MODE_EBH_QUAD, clkdivs[d], saved_mode, saved_div, saved_timing,
                 g_bench_samples, &r);
        char key[REC_LABEL_LEN];
        snprintf(key, sizeof key, "quad_div%u", clkdivs[d]);
        if (!r.measured) {
            rec_text(ctx, key, "EBh quad (1-4-4), div %u (SCK %lu kHz): UNSTABLE - %lu bad 4 KB blocks in "
                               "%u x 128 KB (first: block %lu)",
                     clkdivs[d], (unsigned long)(ctx->clk_khz / clkdivs[d]), (unsigned long)r.bad_blocks,
                     PASSES, (unsigned long)r.first_bad);
            snprintf(key, sizeof key, "quad_div%u_bad_blocks", clkdivs[d]);
            rec_value(ctx, key, UNIT_COUNT, r.bad_blocks);
            continue;
        }
        clean++;
        record_mode(ctx, key, "EBh quad (1-4-4)", clkdivs[d], &r);
    }
    rec_value(ctx, "quad_dividers_clean", UNIT_COUNT, clean);

    // The ROM call configured BOTH chip selects: put the PSRAM window back.
    if (g_psram.present && default_ok) {
        bool ok = psram_reapply();
        rec_text(ctx, "psram_window", "%s", ok ? "restored" : "RESTORE FAILED - PSRAM unusable until reset");
        if (!ok) rec_error(ctx, "psram_restore", 0, 1, 0);
    }

    // 4. Clear QE again if this test set it, and verify the default mode once more.
    test_result_t res = qe_restore(ctx, &qe) ? TR_PASS : TR_FAIL;
    run_mode(false, saved_mode, saved_div, saved_mode, saved_div, saved_timing, g_bench_samples, &r);
    if (!r.measured || qmi_hw->m[0].timing != saved_timing || current_mode() != saved_mode) {
        rec_error(ctx, "restore_failed", r.first_bad, saved_timing, qmi_hw->m[0].timing);
        res = TR_FAIL;
    } else {
        rec_text(ctx, "restore", "default mode back and verified (%u x 128 KB)", PASSES);
    }
    return res;
}

// Continuous read mode (432): the chip skips the 8-clock command byte on every access.
// The boot ROM sends 0xEB / 0xBB on every cache miss; the RP2040's boot stage 2 does not,
// which is most of why its miss is the fastest of the three boards (WORKLOG 2026-11-07).
// Frame model: miss = (SCK cycles per 8-byte fill) x divider + ~5 cycles; continuous read
// takes 8 SCK cycles off the frame.
test_result_t test_flash_continuous_read(test_ctx_t *ctx) {
    if (!gate_ok(ctx)) return TR_SKIPPED;

    saved_qmi_t sv;
    save_qmi(&sv);
    rec_text(ctx, "default_mode", "%s, clock divider %u (SCK %lu kHz), rcmd 0x%04lx, rfmt 0x%08lx",
             mode_name(sv.mode), sv.div, (unsigned long)(sv.div ? ctx->clk_khz / sv.div : 0),
             (unsigned long)sv.rcmd, (unsigned long)sv.rfmt);

    // Quad needs QE; dual does not, so dual is tried even when QE cannot be set.
    qe_state_t qe;
    qe_outcome_t qo = qe_ensure(ctx, &qe);
    if (qo == QE_ERROR) return TR_FAIL;
    const bool quad_ok = qo != QE_NOT_POSSIBLE;

    xip_cache_clean_all();      // write back any cached PSRAM data before invalidating
    take_reference();
    const uint32_t probe_ref = read_word0();
    mode_result_t r;
    run_mode(false, sv.mode, sv.div, sv.mode, sv.div, sv.timing, g_bench_samples, &r);
    bool default_ok = r.measured;
    if (default_ok) {
        record_mode(ctx, "default", mode_name(sv.mode), sv.div, &r);
    } else {
        rec_error(ctx, "default_mode_read", r.first_bad, 0, r.bad_blocks);
    }

    static const struct {
        bootrom_xip_mode_t mode;
        uint8_t div;
        const char *key;
        const char *desc;
    } trials[] = {
        {BOOTROM_XIP_MODE_EBH_QUAD, 3, "cont_quad_div3", "EBh quad (1-4-4), continuous read"},
        {BOOTROM_XIP_MODE_EBH_QUAD, 2, "cont_quad_div2", "EBh quad (1-4-4), continuous read"},
        {BOOTROM_XIP_MODE_BBH_DUAL, 3, "cont_dual_div3", "BBh dual (1-2-2), continuous read"},
        {BOOTROM_XIP_MODE_BBH_DUAL, 2, "cont_dual_div2", "BBh dual (1-2-2), continuous read"},
    };
    unsigned clean = 0;
    for (unsigned i = 0; i < sizeof trials / sizeof trials[0] && default_ok; i++) {
        char key[REC_LABEL_LEN];
        if (trials[i].mode == BOOTROM_XIP_MODE_EBH_QUAD && !quad_ok) {
            rec_text(ctx, trials[i].key, "not tried: quad needs QE, which could not be set");
            continue;
        }
        guard_keepalive();
        cont_result_t c;
        run_continuous(trials[i].mode, trials[i].div, RXDELAY_ROM, &sv, probe_ref, PASSES, true, g_bench_samples,
                       &c);
        if (!c.has_mode_byte) {
            rec_text(ctx, trials[i].key, "not tried: the ROM's %s format has no mode byte (rfmt 0x%08lx)",
                     mode_name(trials[i].mode), (unsigned long)c.rom_rfmt);
            continue;
        }
        if (!c.engaged) {
            rec_text(ctx, trials[i].key, "%s, div %u: NOT ENGAGED - first read without the command byte "
                                         "returned 0x%08lx, expected 0x%08lx (the chip ignored mode byte "
                                         "A0h); nothing else was read",
                     trials[i].desc, trials[i].div, (unsigned long)c.probe, (unsigned long)probe_ref);
            continue;
        }
        if (!c.r.measured) {
            rec_text(ctx, trials[i].key, "%s, div %u (SCK %lu kHz): UNSTABLE - %lu bad 4 KB blocks in "
                                         "%u x 128 KB (first: block %lu)",
                     trials[i].desc, trials[i].div, (unsigned long)(ctx->clk_khz / trials[i].div),
                     (unsigned long)c.r.bad_blocks, PASSES, (unsigned long)c.r.first_bad);
            snprintf(key, sizeof key, "%s_bad_blocks", trials[i].key);
            rec_value(ctx, key, UNIT_COUNT, c.r.bad_blocks);
            continue;
        }
        clean++;
        record_mode(ctx, trials[i].key, trials[i].desc, trials[i].div, &c.r);
        snprintf(key, sizeof key, "%s_rom_rcmd", trials[i].key);
        rec_value(ctx, key, UNIT_HEX, c.rom_rcmd);
    }
    rec_value(ctx, "continuous_trials_clean", UNIT_COUNT, clean);
    rec_text(ctx, "method", "one read with the command byte and mode byte A0h, then reads without the "
                            "command byte (as the SDK's boot2_w25q080); exit through the ROM's "
                            "flash_exit_xip, then the default mode and its exact registers are restored");

    // The ROM calls configured BOTH chip selects: put the PSRAM window back.
    if (g_psram.present && default_ok) {
        bool ok = psram_reapply();
        rec_text(ctx, "psram_window", "%s", ok ? "restored" : "RESTORE FAILED - PSRAM unusable until reset");
        if (!ok) rec_error(ctx, "psram_restore", 0, 1, 0);
    }

    test_result_t res = qe_restore(ctx, &qe) ? TR_PASS : TR_FAIL;
    run_mode(false, sv.mode, sv.div, sv.mode, sv.div, sv.timing, g_bench_samples, &r);
    if (!r.measured || qmi_hw->m[0].timing != sv.timing || qmi_hw->m[0].rfmt != sv.rfmt ||
        qmi_hw->m[0].rcmd != sv.rcmd) {
        rec_error(ctx, "restore_failed", r.first_bad, sv.rfmt, qmi_hw->m[0].rfmt);
        res = TR_FAIL;
    } else {
        rec_text(ctx, "restore", "default mode back (timing, rfmt and rcmd identical) and verified "
                                 "(%u x 128 KB)", PASSES);
    }
    return res;
}


// flash.sck_limit (433): the fastest flash clock at this operating point, and its RX
// delay window. Same gate and QE handling as 431/432; runs at every operating point.
// The mode is the fastest one the chip takes: quad continuous read if QE is (or can be)
// set, dual continuous read otherwise. For each divider 1-3 whose SCK is within
// PICOBENCH_FLASH_SCK_LIMIT_KHZ, every RX delay 0-7 is tried (SCAN_PASSES x 128 KB,
// stopping at the first bad block). The smallest divider with a working delay, at the
// centre of its longest window, then gets the full check (PASSES x 128 KB) and is
// measured. A wrong RX delay only changes where the RP2350 samples the data: what the
// chip receives is the same, and the mode byte A0h keeps WREN from ever forming (see
// run_continuous()).
test_result_t test_flash_sck_limit(test_ctx_t *ctx) {
    if (!gate_ok(ctx)) return TR_SKIPPED;

    saved_qmi_t sv;
    save_qmi(&sv);
    qe_state_t qe;
    qe_outcome_t qo = qe_ensure(ctx, &qe);
    if (qo == QE_ERROR) return TR_FAIL;
    const bool quad = qo != QE_NOT_POSSIBLE;
    const bootrom_xip_mode_t mode = quad ? BOOTROM_XIP_MODE_EBH_QUAD : BOOTROM_XIP_MODE_BBH_DUAL;
    const char *desc = quad ? "EBh quad (1-4-4), continuous read" : "BBh dual (1-2-2), continuous read";
    rec_text(ctx, "mode", "%s%s", desc, quad ? "" : " (quad needs QE, which could not be set)");

    xip_cache_clean_all();      // write back any cached PSRAM data before invalidating
    take_reference();
    const uint32_t probe_ref = read_word0();
    mode_result_t r;
    run_mode(false, sv.mode, sv.div, sv.mode, sv.div, sv.timing, g_bench_samples, &r);
    const bool default_ok = r.measured;
    if (!default_ok) rec_error(ctx, "default_mode_read", r.first_bad, 0, r.bad_blocks);

    uint8_t best_div = 0, best_rx = 0;
    for (uint8_t div = 1; div <= 3 && default_ok; div++) {
        char key[REC_LABEL_LEN], list[32];
        const uint32_t sck = ctx->clk_khz / div;
        snprintf(key, sizeof key, "div%u", div);
        if (sck > PICOBENCH_FLASH_SCK_LIMIT_KHZ) {
            rec_text(ctx, key, "SCK %lu kHz: not tried, above the %lu kHz limit", (unsigned long)sck,
                     (unsigned long)PICOBENCH_FLASH_SCK_LIMIT_KHZ);
            continue;
        }
        uint32_t mask = 0;
        bool no_mode_byte = false;
        for (uint8_t rx = 0; rx <= 7; rx++) {
            guard_keepalive();
            cont_result_t c;
            run_continuous(mode, div, rx, &sv, probe_ref, SCAN_PASSES, false, g_bench_samples, &c);
            if (!c.has_mode_byte) { no_mode_byte = true; break; }
            if (c.engaged && c.r.bad_blocks == 0) mask |= 1u << rx;
        }
        if (no_mode_byte) {
            rec_text(ctx, key, "not tried: the ROM's format for this mode has no mode byte");
            break;
        }
        mask_text(list, sizeof list, mask, 7);
        rec_text(ctx, key, "SCK %lu kHz: RX delay %s of 0-7 read correctly (%u x 128 KB each)",
                 (unsigned long)sck, list, SCAN_PASSES);
        snprintf(key, sizeof key, "div%u_rx_ok_mask", div);
        rec_value(ctx, key, UNIT_HEX, mask);
        if (!best_div && mask) {
            unsigned first, len = longest_run(mask, &first);
            best_div = div;
            best_rx = (uint8_t)(first + (len - 1u) / 2u);
        }
    }

    if (best_div) {
        guard_keepalive();
        cont_result_t c;
        run_continuous(mode, best_div, best_rx, &sv, probe_ref, PASSES, true, g_bench_samples, &c);
        rec_value(ctx, "best_div", UNIT_COUNT, best_div);
        rec_value(ctx, "best_rxdelay", UNIT_COUNT, best_rx);
        rec_value(ctx, "best_sck", UNIT_KHZ, ctx->clk_khz / best_div);
        if (c.engaged && c.r.measured) {
            record_mode(ctx, "best", desc, best_div, &c.r);
        } else {
            rec_text(ctx, "best", "div %u, RX delay %u: UNSTABLE in the full check - %lu bad 4 KB blocks in "
                                  "%u x 128 KB", best_div, best_rx, (unsigned long)c.r.bad_blocks, PASSES);
        }
    } else if (default_ok) {
        rec_text(ctx, "best", "no divider and RX delay read correctly");
    }

    // The ROM calls configured BOTH chip selects: put the PSRAM window back.
    if (g_psram.present && default_ok) {
        bool ok = psram_reapply();
        rec_text(ctx, "psram_window", "%s", ok ? "restored" : "RESTORE FAILED - PSRAM unusable until reset");
        if (!ok) rec_error(ctx, "psram_restore", 0, 1, 0);
    }

    test_result_t res = qe_restore(ctx, &qe) ? TR_PASS : TR_FAIL;
    run_mode(false, sv.mode, sv.div, sv.mode, sv.div, sv.timing, g_bench_samples, &r);
    if (!r.measured || qmi_hw->m[0].timing != sv.timing || qmi_hw->m[0].rfmt != sv.rfmt ||
        qmi_hw->m[0].rcmd != sv.rcmd) {
        rec_error(ctx, "restore_failed", r.first_bad, sv.timing, qmi_hw->m[0].timing);
        res = TR_FAIL;
    } else {
        rec_text(ctx, "restore", "default mode back (timing, rfmt and rcmd identical) and verified");
    }
    return res;
}

// ---- flash_fast.h (overclocking explorer) -------------------------------------------

static bool s_fast_ready, s_fast_qe;
static uint32_t s_fast_probe_ref;

void flash_fast_prepare(void) {
    s_fast_qe = (read_sr(0x35) & QE_BIT) != 0;
    xip_cache_clean_all();      // write back any cached PSRAM data before invalidating
    take_reference();
    s_fast_probe_ref = read_word0();
    s_fast_ready = true;
}

void flash_fast_probe(uint32_t clk_khz, uint32_t sck_max_khz, flash_fast_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s_fast_ready) {
        out->why = "not prepared";
        return;
    }
    if (!s_fast_qe) {
        out->why = "QE clear: quad needs a status-register write, which the explorer never does";
        return;
    }
    saved_qmi_t sv;
    save_qmi(&sv);
    uint32_t div = (clk_khz + sck_max_khz - 1u) / sck_max_khz;
    if (div < 1u) div = 1u;
    if (div > 255u) div = 255u;
    out->tried = true;
    out->div = (uint16_t)div;
    out->sck_khz = clk_khz / div;
    xip_cache_clean_all();
    cont_result_t c;
    for (uint8_t rx = 0; rx <= 7; rx++) {
        run_continuous(BOOTROM_XIP_MODE_EBH_QUAD, (uint8_t)div, rx, &sv, s_fast_probe_ref, 2, false,
                       g_bench_samples, &c);
        if (!c.has_mode_byte) {
            out->tried = false;
            out->why = "the ROM's quad format has no mode byte";
            return;
        }
        if (c.engaged && c.r.bad_blocks == 0) out->rx_mask |= (uint8_t)(1u << rx);
    }
    if (!out->rx_mask) return;
    unsigned first, len = longest_run(out->rx_mask, &first);
    out->rx = (uint8_t)(first + (len - 1u) / 2u);
    run_continuous(BOOTROM_XIP_MODE_EBH_QUAD, (uint8_t)div, out->rx, &sv, s_fast_probe_ref, SCAN_PASSES, true,
                   g_bench_samples, &c);
    if (c.engaged && c.r.measured) {
        stats_t st;
        stats_from_samples(g_bench_samples, MISS_SAMPLES, &st);
        out->verified = true;
        out->miss_p50 = st.p50;
        out->miss_max = st.max;
        out->read4k = c.r.read_cycles;
    }
}

#endif
