/*
 * User buttons that request a new test run while the report disk is shown.
 *
 *   BOOTSEL (all boards): read through the QSPI chip-select pad. This briefly takes
 *     the flash off the bus, so the read runs from RAM with IRQs off (~10 us).
 *   KEY on GPIO23 (RP2350B only, WeAct board): polarity-agnostic - "pressed" means the
 *     level differs from the one sampled at boot. Not used on RP2350A/RP2040, where
 *     GPIO23 is the Pico's SMPS mode pin.
 *
 * Note: BOOTSEL held at power-up enters the ROM bootloader, so it only works as a
 * PicoBench button after boot.
 */

#ifndef PICOBENCH_BUTTON_H
#define PICOBENCH_BUTTON_H

#include <stdbool.h>

void button_init(void);
bool button_pressed(void);

#endif
