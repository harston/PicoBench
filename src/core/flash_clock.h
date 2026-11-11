/*
 * Flash interface clock: divider and read sample delay, changed while the firmware runs
 * from that same flash.
 *
 *   RP2040 : SSI BAUDR (SCK = clk_sys / div, even, >= 2) and RX_SAMPLE_DLY (clk_sys
 *            cycles). BAUDR can only change with the SSI disabled, and XIP cannot fetch
 *            while it is, so the change runs from SRAM with interrupts off.
 *   RP2350 : QMI M0 TIMING CLKDIV (SCK = clk_sys / div), RXDELAY (half clk_sys cycles)
 *            and MIN_DESELECT (clk_sys cycles). CLKDIV may change on the fly, the other
 *            fields only while the QMI is idle - also from SRAM, interrupts off.
 *
 * Nothing here talks to the flash chip: only the host side of the interface changes, so
 * a wrong setting reads wrong data but cannot change the chip. Core 1 must not run from
 * flash during a change (it never does between tests: every test that starts it resets
 * it before returning).
 */

#ifndef PICOBENCH_FLASH_CLOCK_H
#define PICOBENCH_FLASH_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t div;           // SCK = clk_sys / div
    uint8_t rxdelay;        // RP2040: clk_sys cycles; RP2350: half clk_sys cycles
    uint8_t min_deselect;   // RP2350 only: clk_sys cycles with CS high between accesses
} flash_clock_t;

// Saves the setting boot stage 2 (RP2040) or the boot ROM (RP2350) left, together with
// the clk_sys it runs at. Call once at boot, before the clock is changed.
void flash_clock_init(uint32_t clk_khz);

const flash_clock_t *flash_clock_boot(void);
bool flash_clock_is_boot(void);    // the interface is exactly as it was at boot

void flash_clock_get(flash_clock_t *out);
void flash_clock_set(const flash_clock_t *c);
// Writes back the exact boot register contents (RP2350: the whole M0 TIMING register).
void flash_clock_restore_boot(void);

// Only the divider. Making SCK slower this way is safe at both the old and the new
// clk_sys, which is how a clk_sys change is crossed.
void flash_clock_set_div(uint16_t div);

// The smallest legal divider with clk_khz / div <= sck_max_khz.
uint16_t flash_clock_div_for(uint32_t clk_khz, uint32_t sck_max_khz);

// The setting for clk_khz with SCK <= sck_max_khz: that divider, and the boot sample
// delay (RP2040), or the boot sample delay and deselect time kept the same in
// nanoseconds (RP2350).
void flash_clock_for(uint32_t clk_khz, uint32_t sck_max_khz, flash_clock_t *out);

// Checksum of the first `bytes` (multiple of 4) of flash, read through the uncached
// alias from SRAM with interrupts off.
uint32_t flash_clock_checksum(uint32_t bytes);

// Applies c and compares `passes` reads of the first `bytes` with ref. On a mismatch
// the previous setting is put back - still from SRAM, before any flash fetch - and
// false is returned.
bool flash_clock_try(const flash_clock_t *c, uint32_t ref, uint32_t bytes, uint32_t passes);

#endif
