#include "core/button.h"

#include "core/platform.h"
#include "core/sequencer.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/time.h"

#define KEY_GPIO 23

// If the KEY pin still reads low this long after boot, it is not a held button but a
// pin that idles low on this board. It is only reported, never acted on: a press is
// counted solely after a release, so an idle-low pin can never trigger a run.
#define KEY_LOW_REPORT_MS 3000

static bool s_key_enabled;
static bool s_key_low_at_boot;
// Armed per button: each must be seen released once before it counts as pressed, so a
// button that is stuck (or idles low on an unknown board) cannot block the other one.
static bool s_bootsel_armed;
static bool s_key_armed;
static absolute_time_t s_start;

// From pico-examples picoboard/button: float QSPI CS and read it back.
static bool __no_inline_not_in_flash_func(bootsel_pressed)(void) {
    const uint cs_index = 1;
    uint32_t irq = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[cs_index].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) {}
#if PICO_RP2040
    const uint32_t cs_bit = 1u << 1;
#else
    const uint32_t cs_bit = SIO_GPIO_HI_IN_QSPI_CSN_BITS;
#endif
    bool pressed = !(sio_hw->gpio_hi_in & cs_bit);
    hw_write_masked(&ioqspi_hw->io[cs_index].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(irq);
    return pressed;
}

void button_init(void) {
    s_bootsel_armed = false;    // whatever is held at boot must be released first
    s_key_armed = false;
    s_start = get_absolute_time();
    s_key_low_at_boot = false;
    s_key_enabled = platform_chip() == CHIP_RP2350B;   // WeAct KEY; GPIO23 is the SMPS pin elsewhere
    if (!s_key_enabled) return;
    gpio_init(KEY_GPIO);
    gpio_set_dir(KEY_GPIO, GPIO_IN);
    gpio_pull_up(KEY_GPIO);     // active low, the usual wiring (button to GND)
    sleep_ms(2);
}

// True when `down` counts as a press: only after this button has been seen released.
static bool armed_press(bool down, bool *armed) {
    if (*armed) return down;
    if (!down) *armed = true;
    return false;
}

bool button_pressed(void) {
    bool pressed = armed_press(bootsel_pressed(), &s_bootsel_armed);

    if (s_key_enabled) {
        bool key_down = !gpio_get(KEY_GPIO);
        pressed = armed_press(key_down, &s_key_armed) || pressed;
        if (key_down && !s_key_armed &&
            absolute_time_diff_us(s_start, get_absolute_time()) > KEY_LOW_REPORT_MS * 1000) {
            s_key_low_at_boot = true;   // reported in HARDWARE.TXT, nothing else
        }
    }
    return pressed;
}

bool button_key_available(void) {
    return s_key_enabled;
}

const char *button_key_state(void) {
    if (!s_key_enabled) return "not used on this chip (GPIO23 is the SMPS mode pin on Pico boards)";
    if (s_key_low_at_boot)
        return "GPIO23 with pull-up, low since boot: either still held, or this board idles it low";
    return gpio_get(KEY_GPIO) ? "GPIO23 with pull-up, reads high (released, active low)"
                              : "GPIO23 with pull-up, reads low (pressed or held from the previous run)";
}
