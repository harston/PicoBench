#include "storage/flash_layout.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"

extern char __flash_binary_end;

// The SDK hard_asserts every erase/program against PICO_FLASH_SIZE_BYTES (board header).
_Static_assert(PICO_FLASH_SIZE_BYTES >= 16u * 1024 * 1024,
               "PICO_FLASH_SIZE_BYTES must cover the largest supported flash (16 MB)");

static flash_layout_t s_layout;

static uint32_t results_size_for(uint32_t flash_size) {
    if (flash_size >= 16u * 1024 * 1024) return 4u * 1024 * 1024;
    if (flash_size >= 8u * 1024 * 1024) return 2u * 1024 * 1024;
    if (flash_size >= 4u * 1024 * 1024) return 1u * 1024 * 1024;
    return 512u * 1024;
}

void flash_layout_init(void) {
    flash_layout_t *l = &s_layout;
    memset(l, 0, sizeof(*l));

    uint8_t tx[4] = {0x9F, 0, 0, 0};
    uint8_t rx[4] = {0};
    uint32_t irq = save_and_disable_interrupts();
    flash_do_cmd(tx, rx, sizeof(tx));
    restore_interrupts(irq);
    memcpy(l->jedec, &rx[1], 3);

    // JEDEC capacity byte is log2(size) for Winbond, GigaDevice, Macronix, ...
    // Accept 2 MB .. 32 MB; anything else falls back to the smallest supported size.
    uint8_t cap = rx[3];
    if (cap >= 0x15 && cap <= 0x19) {
        l->flash_size = 1u << cap;
        l->size_detected = true;
    } else {
        l->flash_size = 2u * 1024 * 1024;
        l->size_detected = false;
    }

    l->fw_end = (uint32_t)((uintptr_t)&__flash_binary_end - 0x10000000u);
    l->results_size = results_size_for(l->flash_size);
    l->results_base = l->flash_size - l->results_size;
    l->scratch_base = l->results_base - FLASH_SCRATCH_BYTES;
    l->valid = l->scratch_base >= PICOBENCH_FW_MAX_BYTES && l->fw_end <= PICOBENCH_FW_MAX_BYTES &&
               l->flash_size <= PICO_FLASH_SIZE_BYTES;
}

const flash_layout_t *flash_layout(void) {
    return &s_layout;
}
