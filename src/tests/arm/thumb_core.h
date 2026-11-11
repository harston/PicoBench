/*
 * thumb_core - an ARMv4T Thumb interpreter for the ARM code of Harmony
 * cartridges (DPC+, CDF, CDFJ, CDFJ+).
 *
 * The reference is Gopher2600's ARM7TDMI model (ORIG/Gopher2600, d97a9391,
 * hardware/memory/cartridge/arm/thumb.go), NOT the ARM architecture manual:
 * the interpreter is checked bit for bit against calls recorded in Gopher2600
 * (tools/arm_carts/armrec.py), so where the two differ this file follows
 * Gopher2600 and says so. Known differences from real ARM7TDMI silicon:
 *   - shift-by-register uses the whole 32-bit register, not its low byte;
 *   - misaligned word/halfword accesses are aligned down (Gopher2600 also
 *     stops the program there with a memory fault; recorded games never do it);
 *   - condition code 1110 in a conditional branch is "always".
 *
 * Memory model: guest [0, rom_size) is the cartridge image (read-only),
 * guest [0x40000000, 0x40000000 + ram_size) is the Harmony RAM. Everything
 * else goes to the periph_read/periph_write callbacks. A BX to an even
 * address is a switch to ARM state: either the end of the program (the entry
 * LR) or a call into the Harmony driver, handed to the driver callback.
 *
 * Source of truth: PicoA10400 tools/arm_carts/thumb/. PicoBench carries a
 * verbatim copy; keep them identical.
 *
 * Licence: GPL-3.0-or-later, like the rest of the PicoA10400 tree.
 */
#ifndef THUMB_CORE_H
#define THUMB_CORE_H

#include <stdint.h>

/* Where the hot code goes. A firmware defines this to its SRAM section
   attribute (e.g. __not_in_flash("thumb")) before including this file. */
#ifndef THUMB_HOT
#define THUMB_HOT
#endif

/* Where the dispatch table goes. It must be SRAM on a Pico, and leaving out
   "const" is NOT enough: GCC sees that nothing writes the table and puts it in
   .rodata - i.e. flash - anyway (seen in the PicoBench build: tbl.0 at
   0x1003dfd4). A firmware defines this to an explicit .data section. */
#ifndef THUMB_DATA
#define THUMB_DATA
#endif

#define THUMB_RAM_BASE 0x40000000u

enum {
    THUMB_ENDED = 0,        /* BX to the entry LR: normal end of the program */
    THUMB_ERR_BUDGET,       /* more instructions than allowed */
    THUMB_ERR_UNDEFINED,    /* opcode not in ARMv4T Thumb (or SWI) */
    THUMB_ERR_PC,           /* branch outside ROM and RAM, or to an odd address */
    THUMB_ERR_DRIVER,       /* driver callback did not service the address */
    THUMB_ERR_CALLBACK,     /* a callback reported an error */
};

typedef struct thumb_cpu thumb_cpu_t;

/* Return the value read; set cpu->status (non-zero) to stop with an error. */
typedef uint32_t (*thumb_read_fn)(thumb_cpu_t *cpu, uint32_t addr, uint32_t size);
typedef void (*thumb_write_fn)(thumb_cpu_t *cpu, uint32_t addr, uint32_t val, uint32_t size);
/* addr = address of the BX that left Thumb state; r[2], r[3] are the
   arguments. Return 1 if serviced (the callback may write cpu->r[]). */
typedef int (*thumb_driver_fn)(thumb_cpu_t *cpu, uint32_t addr);

struct thumb_cpu {
    uint32_t r[16];         /* r[15]: address of the next instruction */
    uint32_t n, z, c, v;    /* flags as 0/1 (converted on entry and exit) */
    const uint8_t *rom;
    uint32_t rom_size;
    uint8_t *ram;
    uint32_t ram_size;
    uint32_t exit_addr;     /* entry LR with bit 0 cleared */
    uint32_t count;         /* instructions executed by the last thumb_run() */
    uint32_t status;        /* THUMB_* */
    uint32_t err_pc, err_op;
    thumb_read_fn periph_read;      /* may be NULL: reads 0 */
    thumb_write_fn periph_write;    /* may be NULL: writes ignored */
    thumb_driver_fn driver;         /* may be NULL: every driver call fails */
    void *user;
};

/* Run from cpu->r[15] until the program ends, an error occurs or more than
   max_instructions have executed. Returns cpu->status. */
uint32_t thumb_run(thumb_cpu_t *cpu, uint32_t max_instructions);

#endif
