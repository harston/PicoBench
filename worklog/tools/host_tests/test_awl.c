/*
 * Host test of the ARM workload (workload/arm_workload.c, read by src/tests/arm/awl.c):
 * every recorded call of every
 * title must replay bit for bit through thumb_core - the same check
 * cpu.thumb_interp makes on the board. Also exercises awl_prepare_call(), which
 * hotpath.arm_neighbour uses to stage one call for core 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "results/crc32.h"
#include "tests/arm/awl.h"
#include "tests/arm/synth_load.h"

static uint32_t run(thumb_cpu_t *cpu, uint32_t max, uint32_t *cycles) {
    uint32_t st = thumb_run(cpu, max);
    *cycles += cpu->count;      // stand-in "cycles" so the bookkeeping is exercised
    return st;
}

// The synthetic load of hotpath.arm_neighbour (in every build): one run from the
// staged state must give exactly SYNTH_INSTRUCTIONS and SYNTH_RAM_CRC, twice in a row.
static int test_synth(void) {
    uint8_t *rom = malloc(SYNTH_ROM_SIZE), *ram = malloc(SYNTH_RAM_SIZE);
    int bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        thumb_cpu_t cpu;
        synth_load_stage(rom, ram, &cpu);
        uint32_t st = thumb_run(&cpu, 10000000u), crc = crc32_update(0, ram, SYNTH_RAM_SIZE);
        if (st != THUMB_ENDED || cpu.count != SYNTH_INSTRUCTIONS || crc != SYNTH_RAM_CRC) {
            printf("  synthetic load: status %u, %u instructions (want %u), RAM crc %08x (want %08x)\n",
                   st, cpu.count, SYNTH_INSTRUCTIONS, crc, SYNTH_RAM_CRC);
            bad = 1;
        }
    }
    if (!bad) printf("  synthetic load: %u instructions, RAM crc %08x, as expected\n", SYNTH_INSTRUCTIONS, SYNTH_RAM_CRC);
    free(rom);
    free(ram);
    return bad;
}

// With --none: built against arm_workload_none.c, which must look like "no workload".
int main(int argc, char **argv) {
    unsigned n = awl_ntitles(), fails = 0;
    if (test_synth()) return 1;
    if (argc > 1 && !strcmp(argv[1], "--none")) {
        awl_title_t t;
        bool ok = n == 0 && awl_id() == 0 && !awl_title(0, &t);
        printf("test_awl --none: %s\n", ok ? "no workload, as expected" : "FAILED");
        return !ok;
    }
    if (n == 0) { printf("no workload\n"); return 1; }
    printf("  workload crc32 %08x\n", awl_id());
    for (unsigned i = 0; i < n; i++) {
        awl_title_t t;
        awl_title(i, &t);
        uint8_t *rom = malloc(t.rom_size), *ram = malloc(t.ram_size), *ram2 = malloc(t.ram_size);
        awl_load_rom(&t, rom);
        awl_result_t r;
        awl_run(&t, rom, ram, run, &r);
        printf("  %-10s %-6s %2u calls %7u instructions, %u bad", t.name, t.format, r.calls, r.instructions, r.bad);
        if (r.bad) printf(" (first: call %d, kind %u, expected %08x, got %08x)", r.first_bad, r.bad_what,
                          r.bad_expected, r.bad_actual);
        if (r.cycles != r.instructions) { printf(" [cycle sum wrong]"); fails++; }
        // stage the heaviest call and run it twice from a saved copy of RAM
        thumb_cpu_t cpu, cpu2;
        uint32_t instr = 0, ev = 0, best = 0, best_k = 0;
        for (unsigned k = 0; k < t.ncalls; k++) {
            if (!awl_prepare_call(&t, rom, ram, k, &cpu, &instr, &ev)) { printf(" [prepare %u failed]", k); fails++; break; }
            if (ev == 0 && instr > best) { best = instr; best_k = k; }
        }
        if (best) {
            awl_prepare_call(&t, rom, ram, best_k, &cpu, &instr, &ev);
            memcpy(ram2, ram, t.ram_size);
            cpu2 = cpu;
            thumb_run(&cpu, 10000000u);
            uint32_t c1 = cpu.count;
            memcpy(ram, ram2, t.ram_size);
            thumb_run(&cpu2, 10000000u);
            if (cpu.status || cpu2.status || c1 != instr || cpu2.count != instr) {
                printf(" [staged call %u: %u/%u instr, expected %u]", best_k, c1, cpu2.count, instr);
                fails++;
            } else {
                printf(", staged call %u (%u instr) repeats", best_k, instr);
            }
        }
        printf("\n");
        if (r.bad) fails++;
        free(rom); free(ram); free(ram2);
    }
    printf("test_awl: %u titles, %u failed\n", n, fails);
    return fails != 0;
}
