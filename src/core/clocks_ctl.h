/*
 * CPU operating points and clock readback.
 *
 * Operating points stay within the chip's specification (no overclocking):
 *   RP2040 : 48, 96, 120, 125*, 133, 150, 200 MHz
 *   RP2350 : 48, 96, 120, 133, 150* MHz          (* = SDK default, full suite)
 * The RP2040 has been specified up to 200 MHz at a 1.15 V core supply since Pico SDK
 * 2.1 (PICO_USE_FASTEST_SUPPORTED_CLOCK); 150 and 200 MHz run at 1.15 V, as the SDK
 * does, everything else at the 1.10 V default. The RP2350 maximum is 150 MHz.
 *
 * Above 133 MHz the RP2040's flash would run past boot stage 2's proven setting
 * (divider 2), so those switches cross the change at a slow divider and then try
 * divider 2 - verified by reading the flash back - before keeping it (see
 * clocks_set_op() and core/flash_clock.h). The divider in use is recorded with every
 * operating point.
 *
 * The overclocking variant (PICOBENCH_VARIANT=overclock) goes further with
 * clocks_set_custom(); nothing else may.
 *
 * clocks_readback() reads the real configuration back from hardware (frequency
 * counter + PLL and clock-mux registers) rather than trusting what was requested.
 */

#ifndef PICOBENCH_CLOCKS_CTL_H
#define PICOBENCH_CLOCKS_CTL_H

#include <stdint.h>
#include <stdbool.h>

#include "core/flash_clock.h"

#define CLOCKS_MAX_OPS 7        // the most operating points of any chip
#define CLOCKS_OP_NONE 0xFFu    // clocks_current_op() after clocks_set_custom()

typedef struct {
    uint32_t refdiv;
    uint32_t fbdiv;
    uint32_t postdiv1;
    uint32_t postdiv2;
    uint32_t vco_khz;          // computed from XOSC and dividers
    uint32_t out_khz;          // computed output
    bool locked;
} pll_readback_t;

// Measured frequencies (kHz, from the frequency counter; ~1 kHz resolution).
typedef struct {
    uint32_t requested_khz;
    uint32_t sys_khz;
    uint32_t ref_khz;
    uint32_t peri_khz;
    uint32_t usb_khz;
    uint32_t adc_khz;
    uint32_t aux_khz;          // RP2040: clk_rtc, RP2350: clk_hstx
    uint32_t xosc_khz;         // nominal crystal frequency (XOSC_HZ)
    pll_readback_t sys_pll;
    pll_readback_t usb_pll;
    // Raw clock-generator registers: source selection and divider per clock.
    uint32_t sys_ctrl, sys_div;
    uint32_t ref_ctrl, ref_div;
    uint32_t peri_ctrl, peri_div;
    uint32_t usb_ctrl, usb_div;
    uint32_t adc_ctrl, adc_div;
    // Flash interface: RP2040 SSI BAUDR divider; RP2350 QMI M0 TIMING register.
    uint32_t flash_if_reg;
    uint32_t flash_if_div;     // clk_sys / flash_if_div = serial clock
    uint32_t flash_if_rxdelay; // RP2040 RX_SAMPLE_DLY (cycles), RP2350 RXDELAY (half cycles)
    uint32_t vreg_mv;          // core supply setting
} clock_readback_t;

// Call once at boot, before anything changes clk_sys: notes the core voltage the chip
// came up with and puts it back to the default (a watchdog reset does not reset the
// regulator, so a crashed overclocking step could leave it raised), and saves the boot
// flash interface setting.
void clocks_init(void);

unsigned clocks_op_count(void);
uint32_t clocks_op_khz(unsigned op);
unsigned clocks_default_op(void);
unsigned clocks_current_op(void);
uint32_t clocks_op_vreg_mv(unsigned op);

// Switch clk_sys to operating point `op`; re-clocks the LED PWM. Returns false if
// the frequency cannot be produced exactly (the clock is left unchanged).
bool clocks_set_op(unsigned op);

// Overclocking variant only: clk_sys = khz at core voltage vreg_mv (at most 1.30 V, the
// regulator's limit unless it is unlocked - which this firmware never does), flash SCK
// at most flash_sck_max_khz with the boot sample timing kept in nanoseconds. The PSRAM
// window is not touched (psram_retime_direct()). Returns false, unchanged, if the
// frequency cannot be produced. Leave with clocks_set_op().
bool clocks_set_custom(uint32_t khz, uint32_t vreg_mv, uint32_t flash_sck_max_khz, flash_clock_t *flash_out);

// Core voltage: what the chip booted with (before clocks_init), and now.
uint32_t clocks_boot_vreg_mv(void);
uint32_t clocks_vreg_mv(void);

// Nearest frequency to khz (within +-max_off_khz, in 1 MHz steps) the PLL can make from
// the crystal exactly; 0 if none.
uint32_t clocks_attainable_khz(uint32_t khz, uint32_t max_off_khz);

void clocks_readback(clock_readback_t *out);

const char *clocks_src_name_sys(uint32_t ctrl);
const char *clocks_src_name_ref(uint32_t ctrl);
const char *clocks_src_name_aux(const char *clock, uint32_t ctrl);

#endif
