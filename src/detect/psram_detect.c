/*
 * PSRAM auto-detection (RP2350 only). Nothing is assumed: every GPIO that can carry
 * XIP_CS1 (0, 8, 19, and 47 on RP2350B) is tried in turn, board-profile hint first.
 *
 * Per pin: select GPIO_FUNC_XIP_CS1, READ ID (0x9F + 3 address bytes) on QMI CS1 and
 * record the raw manufacturer / KGD / EID bytes. A chip is recognised when KGD is
 * 0x5D (APS6404 / ESP-PSRAM / ISSI); the SDK's psram_eid_to_size() gives the size.
 * If found, the PSRAM is initialised with the SDK default profile (<= 133 MHz) and a
 * short sanity check writes and reads back both ends of the array. Deep tests are
 * phase 6. Runs as a TF_RISKY stage (watchdog): a hang becomes TEST_CRASHED.
 */

#include <stdio.h>
#include <string.h>

#include "tests/tests.h"
#include "core/platform.h"
#include "detect/board.h"
#include "detect/psram_info.h"
#include "results/record.h"

#if !PICO_RP2040
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#endif

psram_info_t g_psram;

#if PICO_RP2040

test_result_t test_psram_detect(test_ctx_t *ctx) {
    (void)ctx;
    return TR_NA;
}

bool psram_reapply(void) {
    return false;
}

uint32_t psram_retime_direct(uint32_t clk_hz, uint32_t max_sck_hz) {
    (void)clk_hz;
    (void)max_sck_hz;
    return 0;
}

bool psram_resume(uint32_t run_nonce) {
    (void)run_nonce;
    return false;
}

#else

bool psram_reapply(void) {
    if (!g_psram.present) return false;
    int rc = psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ, PICO_DEFAULT_PSRAM_MAX_SELECT,
                                    PICO_DEFAULT_PSRAM_MIN_DESELECT);
    if (rc == PICO_OK) {
        uint32_t irq = save_and_disable_interrupts();
        rc = psram_reinitialize();
        restore_interrupts(irq);
    }
    if (rc != PICO_OK) return false;
    xip_cache_invalidate_all();
    g_psram.initialised = true;
    return true;
}

static void __no_inline_not_in_flash_func(write_m1_timing)(uint32_t timing) {
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t i = 0; i < 1024u; i++) __asm volatile("nop");   // let a flash access in flight finish
    qmi_hw->m[1].timing = timing;
    restore_interrupts(irq);
}

uint32_t psram_retime_direct(uint32_t clk_hz, uint32_t max_sck_hz) {
    if (!g_psram.present || !g_psram.initialised || !clk_hz || !max_sck_hz) return 0;
    // psram_configure_params() of SDK 2.3.0, rule for rule: divisor 1 not above 100 MHz,
    // one more half cycle of RX delay when SCK is above 100 MHz.
    uint32_t divisor = (clk_hz + max_sck_hz - 1u) / max_sck_hz;
    if (divisor == 1u && clk_hz > 100000000u) divisor = 2u;
    uint32_t rxdelay = divisor;
    if (clk_hz / divisor > 100000000u) rxdelay += 1u;
    uint32_t period_fs = (uint32_t)(1000000000000000ull / clk_hz);
    uint32_t max_select = (uint32_t)(((uint64_t)PICO_DEFAULT_PSRAM_MAX_SELECT * 1000000ull) /
                                     (64ull * (uint64_t)period_fs));
    uint32_t need = (PICO_DEFAULT_PSRAM_MIN_DESELECT * 1000000u + (period_fs - 1u)) / period_fs;
    uint32_t min_deselect = need > (divisor + 1u) / 2u ? need - (divisor + 1u) / 2u : 0u;
    if (divisor > 255u || rxdelay > 7u || max_select > 63u || min_deselect > 31u) return 0;
    uint32_t timing = 1u << QMI_M1_TIMING_COOLDOWN_LSB |
                      QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                      max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                      min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                      rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                      divisor << QMI_M1_TIMING_CLKDIV_LSB;
    xip_cache_clean_all();   // dirty lines go back with the timing they were read with
    write_m1_timing(timing);
    return divisor;
}

// READ ID on CS1 with the pin already routed; returns MFID/KGD/EID bytes.
static void read_id(uint8_t out[3]) {
    flash_devinfo_size_t prev = flash_devinfo_get_cs_size(1);
    // A non-zero CS1 size makes the boot ROM send the XIP exit sequence to CS1 too.
    flash_devinfo_set_cs_size(1, FLASH_DEVINFO_SIZE_8K);
    uint8_t tx[8] = {0x9F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t rx[8] = {0};
    uint32_t irq = save_and_disable_interrupts();
    flash_do_cmd_cs(tx, rx, sizeof(tx), 1);
    restore_interrupts(irq);
    flash_devinfo_set_cs_size(1, prev);
    out[0] = rx[4];   // manufacturer ID (0x0D = AP Memory)
    out[1] = rx[5];   // known good die (0x5D = pass)
    out[2] = rx[6];   // EID: density and generation
}

#define PSRAM_KEEP_MAGIC 0x50535241u   // "PSRA"
// What psram.detect found in this run, kept across a watchdog reset (see psram_resume()).
static psram_info_t __uninitialized_ram(s_keep);
static uint32_t __uninitialized_ram(s_keep_tag);

static test_result_t detect(test_ctx_t *ctx);

static uint32_t keep_tag(uint32_t run_nonce) {
    return PSRAM_KEEP_MAGIC ^ run_nonce;
}

test_result_t test_psram_detect(test_ctx_t *ctx) {
    test_result_t r = detect(ctx);
    s_keep = g_psram;
    s_keep_tag = keep_tag(record_run_state()->run_nonce);
    return r;
}

bool psram_resume(uint32_t run_nonce) {
    if (s_keep_tag != keep_tag(run_nonce) || !s_keep.present || !s_keep.initialised) return false;
    g_psram = s_keep;
    g_psram.initialised = false;
    flash_devinfo_set_cs_gpio(1, g_psram.cs_gpio);
    gpio_set_function(g_psram.cs_gpio, GPIO_FUNC_XIP_CS1);
    flash_devinfo_set_cs_size(1, flash_devinfo_bytes_to_size(g_psram.size));
    uint8_t id[3];
    read_id(id);
    if (id[1] != s_keep.id[1] || id[2] != s_keep.id[2] || !psram_reapply()) {
        memset(&g_psram, 0, sizeof(g_psram));
        flash_devinfo_set_cs_size(1, FLASH_DEVINFO_SIZE_NONE);
        return false;
    }
    return true;
}

static test_result_t detect(test_ctx_t *ctx) {
    memset(&g_psram, 0, sizeof(g_psram));
    uint8_t pins[4];
    unsigned n = 0;
    int hint = board_profile()->psram_cs_hint;
    if (hint >= 0) pins[n++] = (uint8_t)hint;
    static const uint8_t candidates[] = {0, 8, 19, 47};
    for (unsigned i = 0; i < sizeof(candidates); i++) {
        uint8_t p = candidates[i];
        if (p == 47 && platform_chip() != CHIP_RP2350B) continue;   // QFN60 has no GPIO47
        if ((int)p == hint) continue;
        pins[n++] = p;
    }

    for (unsigned i = 0; i < n && !g_psram.present; i++) {
        uint8_t p = pins[i];
        gpio_function_t prev = gpio_get_function(p);
        flash_devinfo_set_cs_gpio(1, p);
        gpio_set_function(p, GPIO_FUNC_XIP_CS1);
        uint8_t id[3];
        read_id(id);
        size_t size = psram_eid_to_size(id[1], id[2]);
        char key[24];
        snprintf(key, sizeof(key), "probe_gpio%u", p);
        rec_text(ctx, key, "MFID %02x KGD %02x EID %02x -> %s", id[0], id[1], id[2],
                 size ? "PSRAM found" : "nothing");
        if (size) {
            g_psram.present = true;
            g_psram.cs_gpio = p;
            g_psram.size = (uint32_t)size;
            memcpy(g_psram.id, id, 3);
        } else {
            gpio_set_function(p, prev);
        }
    }

    if (!g_psram.present) {
        flash_devinfo_set_cs_size(1, FLASH_DEVINFO_SIZE_NONE);
        rec_text(ctx, "psram", "NOT PRESENT (no chip answered on GPIO 0/8/19%s)",
                 platform_chip() == CHIP_RP2350B ? "/47" : "");
        return TR_NA;
    }

    rec_text(ctx, "psram", "present on GPIO%u", g_psram.cs_gpio);
    rec_text(ctx, "vendor", "%s", g_psram.id[0] == 0x0D ? "AP Memory (APS6404 class)"
                                  : g_psram.id[0] == 0x9D ? "ISSI" : "unknown (KGD ok)");
    rec_value(ctx, "size", UNIT_BYTES, g_psram.size);

    // Initialise with the SDK default profile (max 133 MHz, 8 us max select, 18 ns deselect).
    flash_devinfo_set_cs_size(1, flash_devinfo_bytes_to_size(g_psram.size));
    int rc = psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ, PICO_DEFAULT_PSRAM_MAX_SELECT,
                                    PICO_DEFAULT_PSRAM_MIN_DESELECT);
    uint32_t irq = save_and_disable_interrupts();
    if (rc == PICO_OK) rc = psram_reinitialize();
    restore_interrupts(irq);
    if (rc != PICO_OK) {
        rec_error(ctx, "psram_init", 0, PICO_OK, (uint32_t)rc);
        return TR_FAIL;
    }
    g_psram.initialised = true;
    rec_text(ctx, "init", "SDK default profile: max %lu MHz, max select %u ns, min deselect %u ns",
             (unsigned long)(PICO_DEFAULT_PSRAM_MAX_FREQ / 1000000u), PICO_DEFAULT_PSRAM_MAX_SELECT,
             PICO_DEFAULT_PSRAM_MIN_DESELECT);

    // Sanity: first and last words through the uncached alias (real chip access).
    volatile uint32_t *base = (volatile uint32_t *)PSRAM_NOCACHE_BASE;
    volatile uint32_t *last = (volatile uint32_t *)(PSRAM_NOCACHE_BASE + g_psram.size - 4u);
    const uint32_t a = 0xA5C3F00Fu, b = 0x5A3C0FF0u;
    *base = a;
    *last = b;
    uint32_t ra = *base, rb = *last;
    if (ra != a) rec_error(ctx, "first_word", (uint32_t)(uintptr_t)base, a, ra);
    if (rb != b) rec_error(ctx, "last_word", (uint32_t)(uintptr_t)last, b, rb);
    rec_text(ctx, "sanity", "%s", (ra == a && rb == b) ? "first/last word write+read OK" : "FAILED");
    return (ra == a && rb == b) ? TR_PASS : TR_FAIL;
}

#endif
