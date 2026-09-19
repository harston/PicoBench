#include "core/button.h"

#include "core/platform.h"
#include "core/sequencer.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/time.h"

#define KEY_GPIO 23

static bool s_key_enabled;
static bool s_key_idle_level;

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
    s_key_enabled = platform_chip() == CHIP_RP2350B;
    if (!s_key_enabled) return;
    gpio_init(KEY_GPIO);
    gpio_set_dir(KEY_GPIO, GPIO_IN);
    gpio_pull_up(KEY_GPIO);
    sleep_ms(2);
    s_key_idle_level = gpio_get(KEY_GPIO);
}

bool button_pressed(void) {
    if (bootsel_pressed()) return true;
    return s_key_enabled && gpio_get(KEY_GPIO) != s_key_idle_level;
}
