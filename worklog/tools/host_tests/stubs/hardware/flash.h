// Host stub of the Pico SDK flash API (flash simulated in RAM by test_store.c).
#pragma once
#include <stddef.h>
#include <stdint.h>
#define FLASH_PAGE_SIZE (1u << 8)
#define FLASH_SECTOR_SIZE (1u << 12)
void flash_range_erase(uint32_t flash_offs, size_t count);
void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count);
