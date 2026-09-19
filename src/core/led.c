#include "core/led.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

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

// ---- Status protocol ---------------------------------------------------------------

#define BLINK_ON_US   80000
#define BLINK_GAP_US  220000
#define PATTERN_US    2000000

static alarm_id_t s_alarm;
static unsigned s_pattern_blinks;   // 1 = PASS, 2 = FAIL
static unsigned s_pattern_step;     // even = LED on, odd = LED off

static int64_t pattern_cb(alarm_id_t id, void *user) {
    (void)id; (void)user;
    unsigned steps = s_pattern_blinks * 2;
    unsigned step = s_pattern_step;
    s_pattern_step = (step + 1) % steps;
    if ((step & 1u) == 0) {
        led_on();
        return -BLINK_ON_US;   // negative: relative to the previous target, no drift
    }
    led_off();
    if (step + 1 < steps) return -BLINK_GAP_US;
    return -(int64_t)(PATTERN_US - s_pattern_blinks * BLINK_ON_US - (s_pattern_blinks - 1) * BLINK_GAP_US);
}

static void stop_pattern(void) {
    if (s_alarm > 0) {
        cancel_alarm(s_alarm);
        s_alarm = 0;
    }
}

void led_status(led_status_t status) {
    stop_pattern();
    switch (status) {
    case LED_STATUS_OFF:
        led_off();
        break;
    case LED_STATUS_TESTING:
        led_blink(4000, 50);
        break;
    case LED_STATUS_WRITING:
        led_on();
        break;
    case LED_STATUS_DONE_PASS:
    case LED_STATUS_DONE_FAIL:
        led_off();
        s_pattern_blinks = status == LED_STATUS_DONE_PASS ? 1 : 2;
        s_pattern_step = 0;
        s_alarm = add_alarm_in_ms(500, pattern_cb, NULL, true);
        break;
    }
}
