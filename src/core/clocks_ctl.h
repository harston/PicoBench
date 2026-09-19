/*
 * CPU operating points and clock readback.
 *
 * Operating points never exceed the chip's specified maximum (no overclocking):
 *   RP2040 : 48, 96, 120, 125*, 133 MHz
 *   RP2350 : 48, 96, 120, 133, 150* MHz          (* = SDK default, full suite)
 *
 * clocks_readback() reads the real configuration back from hardware (frequency
 * counter + PLL and clock-mux registers) rather than trusting what was requested.
 */

#ifndef PICOBENCH_CLOCKS_CTL_H
#define PICOBENCH_CLOCKS_CTL_H

#include <stdint.h>
#include <stdbool.h>

#define CLOCKS_MAX_OPS 5

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
} clock_readback_t;

unsigned clocks_op_count(void);
uint32_t clocks_op_khz(unsigned op);
unsigned clocks_default_op(void);
unsigned clocks_current_op(void);

// Switch clk_sys to operating point `op`; re-clocks the LED PWM. Returns false if
// the frequency cannot be produced exactly (the clock is left unchanged).
bool clocks_set_op(unsigned op);

void clocks_readback(clock_readback_t *out);

const char *clocks_src_name_sys(uint32_t ctrl);
const char *clocks_src_name_ref(uint32_t ctrl);
const char *clocks_src_name_aux(const char *clock, uint32_t ctrl);

#endif
