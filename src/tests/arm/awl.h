/*
 * ARM workload: real ARM code of Harmony cartridge games, replayed through the
 * Thumb interpreter and checked against the results the Gopher2600 emulator
 * produced for the same calls.
 *
 * The data is workload/arm_workload.c, generated locally from the user's own ROMs by
 * worklog/tools/arm_workload/awl_tool.py (layout described there) and never
 * committed; without it the build uses arm_workload_none.c. This module
 * only walks it; it does not depend on the Pico SDK, so the host tests
 * (worklog/tools/host_tests) run exactly this code.
 */
#ifndef PICOBENCH_AWL_H
#define PICOBENCH_AWL_H

#include <stdbool.h>
#include <stdint.h>

#include "tests/arm/thumb_core.h"

typedef struct {
    char name[17];
    char format[9];
    uint32_t need_mips_x10;     // MIPS the game needs for a stable frame (x10), 0 = unknown
    uint32_t rom_size, ram_size, exit_addr;
    uint32_t npages;
    const uint32_t *pages;      // npages x {index, 64 words}
    uint32_t ncalls;
    const uint32_t *calls;      // first call record
} awl_title_t;

// Runs one call: must execute thumb_run(cpu, max) and may time it.
// Returns thumb_run's status and adds the cycles it measured to *cycles.
typedef uint32_t (*awl_run_fn)(thumb_cpu_t *cpu, uint32_t max, uint32_t *cycles);

typedef struct {
    uint32_t calls;             // calls run
    uint32_t instructions;      // guest instructions executed (as counted by the interpreter)
    uint64_t cycles;            // sum reported by the run function
    uint32_t max_call_cycles;
    uint32_t bad;               // calls whose result differs from Gopher2600's
    int first_bad;              // index of the first such call, -1 if none
    uint32_t bad_what;          // AWL_BAD_*
    uint32_t bad_expected, bad_actual;
} awl_result_t;

enum { AWL_BAD_STATUS = 1, AWL_BAD_COUNT, AWL_BAD_REG, AWL_BAD_FLAGS, AWL_BAD_RAM, AWL_BAD_EVENTS };

// 0 when the firmware was built without a workload (src/tests/arm/arm_workload_none.c).
unsigned awl_ntitles(void);
// CRC32 of the whole workload, as awl_tool.py prints it ("workload crc32"): two runs
// measured the same code only if this matches. 0 without a workload.
uint32_t awl_id(void);
bool awl_title(unsigned i, awl_title_t *t);

// rom: rom_size bytes, 4-aligned. Zero-filled, then the recorded pages copied in.
void awl_load_rom(const awl_title_t *t, uint8_t *rom);

// Runs calls 0..ncalls-1 in order on ram (ram_size bytes, 4-aligned), starting
// from all-zero RAM, and compares every result.
void awl_run(const awl_title_t *t, const uint8_t *rom, uint8_t *ram, awl_run_fn run, awl_result_t *res);

// Brings ram and *cpu to the state right before call k (runs calls 0..k-1 and
// applies call k's RAM patches). Returns false if an earlier call went wrong.
// The call can then be run - repeatedly, from a saved copy of ram - with
// thumb_run(cpu, ...); cpu->user is left NULL, so it must not need events.
bool awl_prepare_call(const awl_title_t *t, const uint8_t *rom, uint8_t *ram, unsigned k,
                      thumb_cpu_t *cpu, uint32_t *instructions, uint32_t *events);

#endif
