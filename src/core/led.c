#include "core/led.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

typedef enum { LED_MODE_OFF, LED_MODE_ON, LED_MODE_BLINK } led_mode_t;

static unsigned s_pin;
static unsigned s_slice;
static unsigned s_chan;
static led_mode_t s_mode;
static uint32_t s_req_freq_mhz;
static unsigned s_duty_pct;
static uint32_t s_actual_freq_mhz;

void led_init(unsigned pin) {
    s_pin = pin;
    s_slice = pwm_gpio_to_slice_num(pin);
    s_chan = pwm_gpio_to_channel(pin);
    pwm_set_enabled(s_slice, false);
    pwm_set_phase_correct(s_slice, true);
    pwm_set_wrap(s_slice, 0xFFFF);
    pwm_set_chan_level(s_slice, s_chan, 0);
    pwm_set_enabled(s_slice, true);
    gpio_set_function(pin, GPIO_FUNC_PWM);
    s_mode = LED_MODE_OFF;
    s_actual_freq_mhz = 0;
}

static void apply(void) {
    switch (s_mode) {
    case LED_MODE_OFF:
        pwm_set_chan_level(s_slice, s_chan, 0);
        s_actual_freq_mhz = 0;
        return;
    case LED_MODE_ON:
        // Level above wrap keeps the output permanently high.
        pwm_set_wrap(s_slice, 0xFFFE);
        pwm_set_chan_level(s_slice, s_chan, 0xFFFF);
        s_actual_freq_mhz = 0;
        return;
    case LED_MODE_BLINK:
        break;
    }

    // Phase-correct period in clk_sys cycles = 2 * (wrap + 1) * div16 / 16.
    const uint64_t clk = clock_get_hz(clk_sys);
    uint32_t wrap = 0xFFFF;
    uint64_t div16 = (clk * 16u * 1000u) / ((uint64_t)s_req_freq_mhz * 2u * (wrap + 1u));
    if (div16 > 0xFFF) {
        div16 = 0xFFF;                     // slowest possible: 255 + 15/16
    } else if (div16 < 16) {
        div16 = 16;                        // divider 1.0, shorten wrap instead
        uint64_t w = (clk * 1000u) / ((uint64_t)s_req_freq_mhz * 2u);
        wrap = (uint32_t)(w > 1 ? w - 1 : 1);
    }
    pwm_set_clkdiv_int_frac4(s_slice, (uint8_t)(div16 >> 4), (uint8_t)(div16 & 0xF));
    pwm_set_wrap(s_slice, (uint16_t)wrap);
    pwm_set_chan_level(s_slice, s_chan, (uint16_t)(((uint64_t)(wrap + 1u) * s_duty_pct) / 100u));
    s_actual_freq_mhz = (uint32_t)((clk * 16u * 1000u) / (2u * (uint64_t)(wrap + 1u) * div16));
}

void led_off(void) {
    s_mode = LED_MODE_OFF;
    apply();
}

void led_on(void) {
    s_mode = LED_MODE_ON;
    apply();
}

void led_blink(uint32_t freq_mhz, unsigned duty_pct) {
    s_mode = LED_MODE_BLINK;
    s_req_freq_mhz = freq_mhz ? freq_mhz : 1;
    s_duty_pct = duty_pct > 100 ? 100 : duty_pct;
    apply();
}

void led_reclock(void) {
    apply();
}

uint32_t led_actual_freq_mhz(void) {
    return s_actual_freq_mhz;
}

unsigned led_pin(void) {
    return s_pin;
}
