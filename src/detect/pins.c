/*
 * Pin map: which bank-0 GPIOs this board already uses, and which are free for your own
 * hardware (e.g. an Atari cartridge bus).
 *
 * Built from what was detected, not from a fixed table: the PSRAM chip select is only
 * reported as taken when a PSRAM chip actually answered, and the Pico reference-design
 * pins (23/24/29) are only claimed for a genuine Pico / Pico 2 layout - a clone that
 * merely looks like one is flagged instead, with the VSYS measurement as evidence.
 */

#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "core/platform.h"
#include "core/sequencer.h"
#include "detect/board.h"
#include "detect/psram_info.h"
#include "hardware/platform_defs.h"
#include "results/record.h"

#define MAX_GPIO 48u

typedef struct {
    bool taken;
    const char *use;
} pin_use_t;

static pin_use_t s_pins[MAX_GPIO];

static void take(unsigned gpio, const char *use) {
    if (gpio >= MAX_GPIO) return;
    s_pins[gpio].taken = true;
    s_pins[gpio].use = use;
}

// "0-22, 24, 26-47": compact list of the free pins.
static void free_ranges(unsigned count, char *out, size_t n) {
    out[0] = '\0';
    size_t len = 0;
    for (unsigned g = 0; g < count;) {
        if (s_pins[g].taken) { g++; continue; }
        unsigned start = g;
        while (g < count && !s_pins[g].taken) g++;
        int w = start == g - 1u ? snprintf(out + len, n - len, "%s%u", len ? ", " : "", start)
                                : snprintf(out + len, n - len, "%s%u-%u", len ? ", " : "", start, g - 1u);
        if (w < 0 || (size_t)w >= n - len) return;
        len += (size_t)w;
    }
}

test_result_t test_inv_pins(test_ctx_t *ctx) {
    memset(s_pins, 0, sizeof(s_pins));
    const board_profile_t *b = board_profile();
    unsigned count = platform_chip() == CHIP_RP2350B ? 48u : 30u;

    rec_value(ctx, "gpio_count", UNIT_COUNT, count);
    rec_text(ctx, "dedicated_pads", "QSPI (flash and PSRAM clock/data), USB, SWD and RUN have "
                                    "their own pads: they cost no bank-0 GPIO");
    rec_text(ctx, "adc_pins", "%s", count == 48u ? "GPIO 40-47" : "GPIO 26-29");

    // ---- Taken on every supported board ----
    take(b->led_pin, "on-board LED with its series resistor (a permanent load on the pin)");

    // ---- Board specific ----
    switch (b->kind) {
    case BOARD_WEACT_RP2350B:
    case BOARD_RP2350B_OTHER:
        take(23, "KEY button to GND (input with pull-up)");
        break;
    case BOARD_PICO:
    case BOARD_PICO2:
        take(23, "SMPS power-save control output to the regulator (Pico reference design)");
        take(24, "VBUS sense input (Pico reference design)");
        if (g_vsys_divider == 1) take(29, "VSYS/3 divider on ADC3 (measured)");
        break;
    case BOARD_RP2350A_OTHER:
    case BOARD_RP2040_OTHER:
        rec_text(ctx, "clone_warning", "not a Raspberry Pi reference board: GPIO 23/24/29 may be "
                 "wired differently than on a Pico - check this board's schematic. VSYS divider on "
                 "GPIO29: %s", g_vsys_divider == 1 ? "found" : g_vsys_divider == 0 ? "NOT found"
                                                                                  : "not measured");
        if (g_vsys_divider == 1) take(29, "VSYS/3 divider on ADC3 (measured)");
        break;
    }

    // ---- PSRAM chip select: only if a chip actually answered ----
    if (g_psram.present) {
        take(g_psram.cs_gpio, "PSRAM chip select (XIP_CS1): busy on every PSRAM access. "
                              "If the chip stays fitted but unused, hold this pin HIGH - "
                              "a low level selects the PSRAM on the bus shared with flash");
    } else if (b->psram_cs_hint >= 0) {
        rec_text(ctx, "psram_footprint", "GPIO%d has a PSRAM footprint but no chip answered: free "
                 "now, but it becomes the PSRAM chip select if a chip is fitted", b->psram_cs_hint);
    }

    // ---- Report ----
    unsigned taken = 0;
    for (unsigned g = 0; g < count; g++) {
        if (!s_pins[g].taken) continue;
        taken++;
        char key[REC_LABEL_LEN];
        snprintf(key, sizeof(key), "gpio%u", g);
        rec_text(ctx, key, "TAKEN - %s", s_pins[g].use);
    }
    char ranges[160];
    free_ranges(count, ranges, sizeof(ranges));
    rec_value(ctx, "free_count", UNIT_COUNT, count - taken);
    rec_text(ctx, "free_gpio", "%s", ranges);
    rec_text(ctx, "picobench_note", "during a run PicoBench briefly drives the first floating pin "
                                    "among GPIO 28/27/26/22/21/20 (gpio.sio) and releases it");
    return TR_PASS;
}
