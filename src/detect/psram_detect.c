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
#include "hardware/psram.h"
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

test_result_t test_psram_detect(test_ctx_t *ctx) {
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
