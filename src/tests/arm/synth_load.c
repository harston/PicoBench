#include "tests/arm/synth_load.h"

#include <string.h>

#define SYNTH_CODE_AT  0x0100u
#define SYNTH_TABLE_AT 0x1000u
#define SYNTH_EXIT     0x0010u     // any even address that is not code: BX there ends the run

// The program in synth_load.h, as assembled (little-endian halfwords, literal pool last).
static const uint16_t s_code[] = {
    0x2710, 0x480a, 0x490a, 0x2680, 0x7802, 0x7843, 0x021b, 0x431a,
    0x0ad4, 0x00a4, 0x590d, 0x18ad, 0x510d, 0x42aa, 0xd200, 0x800a,
    0x3002, 0x3e01, 0xd1f0, 0x3f01, 0xd1eb, 0x4770,
    0x1000, 0x0000,     // .word 0x00001000
    0x0400, 0x4000,     // .word 0x40000400
};

void synth_load_stage(uint8_t *rom, uint8_t *ram, thumb_cpu_t *cpu) {
    memset(rom, 0, SYNTH_ROM_SIZE);
    memset(ram, 0, SYNTH_RAM_SIZE);
    for (unsigned i = 0; i < sizeof s_code / sizeof s_code[0]; i++) {
        rom[SYNTH_CODE_AT + 2u * i] = (uint8_t)s_code[i];
        rom[SYNTH_CODE_AT + 2u * i + 1u] = (uint8_t)(s_code[i] >> 8);
    }
    // The item table: a fixed xorshift sequence, so every run and every board
    // executes exactly the same instructions.
    uint32_t x = 0x2600A10Bu;
    for (unsigned i = 0; i < 256u; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rom[SYNTH_TABLE_AT + i] = (uint8_t)x;
    }
    memset(cpu, 0, sizeof *cpu);
    cpu->r[14] = SYNTH_EXIT;
    cpu->r[15] = SYNTH_CODE_AT;
    cpu->rom = rom;
    cpu->rom_size = SYNTH_ROM_SIZE;
    cpu->ram = ram;
    cpu->ram_size = SYNTH_RAM_SIZE;
    cpu->exit_addr = SYNTH_EXIT;
}
