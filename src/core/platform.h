/*
 * Minimal chip identification needed by the sequencer from the first boot.
 * Full inventory (bootrom, flash, PSRAM, board profile) is in src/detect/ (phase 3).
 */

#ifndef PICOBENCH_PLATFORM_H
#define PICOBENCH_PLATFORM_H

#include <stdint.h>

// CHIP_* bit (see core/sequencer.h) for the chip we are running on.
uint8_t platform_chip(void);
const char *platform_chip_name(void);   // "RP2040", "RP2350A", "RP2350B"
const char *platform_arch_name(void);   // "Cortex-M0+", "Cortex-M33", "Hazard3 (RV32IMAC+)"
uint32_t platform_chip_id_raw(void);    // SYSINFO CHIP_ID
unsigned platform_chip_revision(void);  // CHIP_ID revision field

#endif
