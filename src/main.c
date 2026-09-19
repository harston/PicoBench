/*
 * PicoBench - Pico Hardware Characterization & Benchmark.
 *
 * Phase 0 skeleton: proves the build for all three platforms and the PWM-driven
 * status LED. The LED walks through its state patterns in a loop:
 *   ~4 Hz blink (tests running) -> solid (flash write) -> PASS -> FAIL.
 * The PASS/FAIL patterns are done in software here; phase 1 moves them to a timer.
 */

#include "pico/stdlib.h"
#include "pico/binary_info.h"

#include "core/led.h"
#include "picobench_version.h"

bi_decl(bi_program_feature("Build ID " PICOBENCH_BUILD_ID_STR));
bi_decl(bi_program_feature("Platform " PICOBENCH_PLATFORM));

// Short blinks every ~2 s: 1 = PASS, 2 = FAIL. Blocking; idle state only.
static void led_signal_blinks(unsigned count, unsigned repeats) {
    for (unsigned r = 0; r < repeats; r++) {
        for (unsigned i = 0; i < count; i++) {
            led_on();
            sleep_ms(80);
            led_off();
            sleep_ms(220);
        }
        sleep_ms(2000 - count * 300);
    }
}

int main(void) {
    led_init(PICO_DEFAULT_LED_PIN);

    for (;;) {
        led_blink(4000, 50);          // tests running
        sleep_ms(5000);
        led_on();                     // writing / verifying flash
        sleep_ms(2000);
        led_off();
        sleep_ms(1000);
        led_signal_blinks(1, 3);      // PASS
        led_signal_blinks(2, 3);      // FAIL
    }
}
