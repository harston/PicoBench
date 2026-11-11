/*
 * The fastest flash setting at an arbitrary clk_sys, for the overclocking explorer
 * (overclock.c). Implemented in flash_quad.c next to the continuous-read machinery of
 * tests 432/433.
 *
 *   RP2350 : quad continuous read (one 0xEB with mode byte A0h, then no command byte),
 *            divider = ceil(clk_sys / sck_max), every RX delay 0-7 tried, the centre of
 *            the longest working window verified again and measured. Only if the chip's
 *            QE bit is already set: the explorer never writes a status register.
 *   RP2040 : boot stage 2 already runs quad continuous read; the smallest even SSI
 *            divider within sck_max, every RX sample delay 0-7, the centre measured.
 *
 * Nothing is written to the flash chip. The flash interface is left exactly as found.
 * On the RP2350 the boot ROM's mode switch also rewrites the PSRAM window (CS1), so
 * PSRAM must not be touched until psram_reapply() has run at the default clock.
 */

#ifndef PICOBENCH_FLASH_FAST_H
#define PICOBENCH_FLASH_FAST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool tried;             // false: not possible here, `why` says so
    const char *why;
    uint16_t div;
    uint32_t sck_khz;
    uint8_t rx_mask;        // RX delays that read back correctly
    uint8_t rx;             // the one measured (centre of the longest window)
    bool verified;          // the measured setting passed the longer check
    uint32_t miss_p50, miss_max;  // cold cache miss, cycles
    uint32_t read4k;        // 4 KB through the uncached alias, cycles (RP2350 only)
} flash_fast_t;

// At the default clock, before the explorer raises it: reference checksums, and (RP2350)
// whether QE is set. Reading the status register goes through flash_do_cmd(), which puts
// the flash window back to the boot setting - right only at the default clock.
void flash_fast_prepare(void);

// At the raised clock. Interrupts may be on; nothing else may use flash or PSRAM.
void flash_fast_probe(uint32_t clk_khz, uint32_t sck_max_khz, flash_fast_t *out);

#endif
