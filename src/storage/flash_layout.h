/*
 * Flash layout. The firmware image is capped at PICOBENCH_FW_MAX_BYTES by the linker
 * (CMake PICO_FLASH_SIZE_BYTES). Everything else is placed from the END of the
 * detected flash, so it never depends on the image size:
 *
 *   0                     firmware (<= 1 MB)
 *   ...                   unused
 *   scratch_base          64 KB scratch for the controlled flash-write test (phase 5)
 *   results_base          results area:
 *                           sector 0  directory slot A
 *                           sector 1  directory slot B
 *                           sector 2+ run data (ring)
 *   flash_size
 *
 * Results area size: 512 KB (2 MB flash), 1 MB (4 MB), 2 MB (8 MB), 4 MB (>= 16 MB).
 *
 * Size detection is READ-ONLY (JEDEC 0x9F, SFDP 0x5A) - it never writes or erases.
 * Counterfeit or mislabelled chips (common on clones) may claim more than they have;
 * their address lines then wrap, so an area placed near the "end" would in fact
 * overwrite the firmware at offset 0. An address-wrap probe compares the bytes at
 * offset 0 with the bytes at each power-of-two offset: if they are identical, the chip
 * is that small, and the smaller size is used. As a last line of defence the results
 * area itself is checked the same way and writing is disabled if it aliases.
 */

#ifndef PICOBENCH_FLASH_LAYOUT_H
#define PICOBENCH_FLASH_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define PICOBENCH_FW_MAX_BYTES   (1024u * 1024u)
#define FLASH_SCRATCH_BYTES      (64u * 1024u)

typedef struct {
    uint32_t flash_size;        // size actually used (JEDEC, reduced if address wrap found)
    uint32_t jedec_size;        // size claimed by the JEDEC capacity byte
    uint32_t alias_size;        // size proven by the address-wrap probe (0 = no wrap seen)
    bool size_detected;         // false: JEDEC capacity unusable, fallback used
    bool results_area_aliased;  // results area maps onto the firmware: NEVER write
    uint8_t jedec[3];           // manufacturer, memory type, capacity
    uint32_t fw_end;            // end of the firmware image (offset)
    uint32_t scratch_base;      // offsets from the start of flash
    uint32_t results_base;
    uint32_t results_size;
    bool valid;                 // layout does not overlap the firmware
} flash_layout_t;

// Reads the JEDEC ID and computes the layout. Call once at boot, before any
// flash-resident code runs on core 1 and before USB is started.
void flash_layout_init(void);
const flash_layout_t *flash_layout(void);

// XIP (cached) address of a flash offset.
#ifdef PICOBENCH_HOST_TEST
extern uint8_t *g_host_flash;   // host unit tests: flash simulated in RAM
static inline const uint8_t *flash_xip_ptr(uint32_t offset) {
    return g_host_flash + offset;
}
#else
static inline const uint8_t *flash_xip_ptr(uint32_t offset) {
    return (const uint8_t *)(uintptr_t)(0x10000000u + offset);
}
#endif

#endif
