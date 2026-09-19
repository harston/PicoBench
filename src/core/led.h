/*
 * Status LED driven by hardware PWM.
 *
 * The blink runs entirely in the PWM slice, so it costs no CPU time and no bus
 * traffic while benchmarks run. The PWM counter is clocked from clk_sys, so
 * led_reclock() must be called after every clk_sys change.
 *
 * Phase-correct mode doubles the period: the slowest blink is
 *   clk_sys / (256 * 2 * 65536)  ->  4.47 Hz at 150 MHz, 3.73 Hz at 125 MHz.
 * "About 4 Hz" is therefore achievable at every supported operating point;
 * requests slower than the hardware minimum are clamped to it.
 */

#ifndef PICOBENCH_LED_H
#define PICOBENCH_LED_H

#include <stdint.h>
#include <stdbool.h>

void led_init(unsigned pin);
void led_off(void);
void led_on(void);

// Blink with the given frequency in milli-hertz and duty cycle in percent.
void led_blink(uint32_t freq_mhz, unsigned duty_pct);

// Re-apply the current mode after a clk_sys change.
void led_reclock(void);

// Frequency actually produced by the hardware for the current blink, in milli-hertz
// (0 when not blinking).
uint32_t led_actual_freq_mhz(void);

unsigned led_pin(void);

#endif
