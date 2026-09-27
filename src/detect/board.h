/*
 * Board profile, inferred at runtime (the same UF2 runs on several boards).
 *
 * Evidence used: chip variant (RP2040 / RP2350A / RP2350B) and detected flash size.
 * The profile only selects defaults (LED pin, user key); every fact the report
 * relies on is measured, not taken from the profile.
 */

#ifndef PICOBENCH_BOARD_H
#define PICOBENCH_BOARD_H

#include <stdint.h>

typedef enum {
    BOARD_WEACT_RP2350B,
    BOARD_RP2350B_OTHER,
    BOARD_PICO2,            // Raspberry Pi reference design
    BOARD_RP2350A_OTHER,    // Pico 2 compatible / clone
    BOARD_PICO,             // Raspberry Pi reference design
    BOARD_RP2040_OTHER,     // e.g. purple clone
} board_kind_t;

typedef struct {
    board_kind_t kind;
    const char *name;
    const char *basis;       // why this profile was chosen
    unsigned led_pin;
    int key_pin;             // -1: none (BOOTSEL only)
    int psram_cs_hint;       // expected PSRAM CS footprint, -1: none
} board_profile_t;

const board_profile_t *board_profile(void);

// Whether a Pico-style VSYS/3 divider was measured on GPIO29 by core.environment:
// -1 = not measured (RP2350B, or the test has not run), 0 = absent, 1 = present.
// Tells a genuine Pico/Pico 2 layout apart from a clone that only looks like one.
extern int8_t g_vsys_divider;

#endif
