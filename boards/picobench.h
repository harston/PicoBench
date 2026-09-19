/*
 * PicoBench board header.
 *
 * One header for every supported board (Pico / purple clone, Pico 2, WeAct RP2350B).
 * It deliberately makes as few assumptions as possible: the real board, flash size,
 * LED pin and PSRAM are detected at runtime. Values here are only the safe common
 * denominator needed to build and boot.
 *
 * Platform (rp2040 / rp2350-arm-s / rp2350-riscv) is chosen with -DPICO_PLATFORM,
 * not here.
 */

#ifndef _BOARDS_PICOBENCH_H
#define _BOARDS_PICOBENCH_H

#define PICOBENCH_BOARD 1

// RP2350: build for the B package (48 GPIO). PICO_RP2350A defaults to 0 in the SDK;
// the firmware checks the real package at runtime before touching GPIO 30-47.

// RP2040 boot stage 2. W25Q080 stage 2 uses standard Winbond quad commands and is
// the one the official Pico uses; the purple clones carry W25Q128-class parts.
// Verified on hardware: boots on both the Pico and the purple clone.
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

// Smallest flash among supported boards. The real size is detected at runtime;
// this only bounds the firmware image in the linker script.
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

// Fallback LED pin. All supported boards use GPIO25; the board profile may override.
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

// Longer crystal start-up wait: some RP2040 clones have slow-starting crystals and
// otherwise fail to boot. Costs only a few ms at power-up.
#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif

#endif
