/*
 * Board profile, inferred at runtime (the same UF2 runs on several boards).
 *
 * Evidence used: chip variant (RP2040 / RP2350A / RP2350B) and detected flash size.
 * The profile only selects defaults (LED pin, user key); every fact the report
 * relies on is measured, not taken from the profile.
 */

#ifndef PICOBENCH_BOARD_H
#define PICOBENCH_BOARD_H

typedef struct {
    const char *name;
    const char *basis;       // why this profile was chosen
    unsigned led_pin;
    int key_pin;             // -1: none (BOOTSEL only)
    int psram_cs_hint;       // expected PSRAM CS footprint, -1: none
} board_profile_t;

const board_profile_t *board_profile(void);

#endif
