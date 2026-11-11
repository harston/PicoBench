/*
 * A synthetic ARM load for hotpath.arm_neighbour: a short Thumb program written for
 * PicoBench (no game code, so it is part of every build, published ones included),
 * run by the same interpreter as the recorded games.
 *
 * What it imitates is the memory traffic of a Harmony game's ARM code, measured in
 * PicoA10400 (CHAMP_ARM.md section 3: 0.29-0.46 data accesses and 8-17 % conditional
 * branches per instruction): this program makes 0.33 data accesses and 0.13 branches
 * per instruction, loads from the cartridge ROM and loads/stores to its RAM, in a
 * 32 KB ROM + 8 KB RAM image like a CDFJ game's. One run is 30772 instructions, about
 * the size of a game's heaviest call (Galagon: 25534). For the neighbour test the interpreter's
 * own traffic (opcode fetch, dispatch table, register file) dominates anyway, and
 * that is identical for any guest code.
 *
 * Source (arm-none-eabi-as -march=armv4t -mthumb, placed at guest 0x0100):
 *
 *   start:  movs r7, #16            @ passes
 *   outer:  ldr  r0, =0x00001000    @ ROM: 256-byte item table
 *           ldr  r1, =0x40000400    @ RAM: 32-word work area
 *           movs r6, #128           @ items per pass
 *   inner:  ldrb r2, [r0, #0]       @ ROM
 *           ldrb r3, [r0, #1]       @ ROM
 *           lsls r3, r3, #8
 *           orrs r2, r3             @ 16-bit item
 *           lsrs r4, r2, #11        @ slot 0..31
 *           lsls r4, r4, #2
 *           ldr  r5, [r1, r4]       @ RAM read
 *           adds r5, r5, r2
 *           str  r5, [r1, r4]       @ RAM write
 *           cmp  r2, r5
 *           bhs  1f
 *           strh r2, [r1, #0]       @ RAM write, data dependent
 *   1:      adds r0, #2
 *           subs r6, #1
 *           bne  inner
 *           subs r7, #1
 *           bne  outer
 *           bx   lr                 @ lr = SYNTH_EXIT: end of the program
 *           .pool
 */
#ifndef PICOBENCH_SYNTH_LOAD_H
#define PICOBENCH_SYNTH_LOAD_H

#include <stdint.h>

#include "tests/arm/thumb_core.h"

#define SYNTH_ROM_SIZE (32u * 1024u)
#define SYNTH_RAM_SIZE (8u * 1024u)

// One run from the staged state, as thumb_core executes it (worked out on the host,
// checked by worklog/tools/host_tests/test_awl.c and by the test on the board).
#define SYNTH_INSTRUCTIONS 30772u
#define SYNTH_RAM_CRC      0x39b123bdu

// Fills rom and ram (4-aligned, SYNTH_ROM_SIZE / SYNTH_RAM_SIZE bytes) and sets *cpu
// up for one run of the program.
void synth_load_stage(uint8_t *rom, uint8_t *ram, thumb_cpu_t *cpu);

#endif
