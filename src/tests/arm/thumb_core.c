/*
 * thumb_core - ARMv4T Thumb interpreter, semantics of Gopher2600's ARM7TDMI.
 * See thumb_core.h for the model and the known differences from silicon.
 *
 * Structure: one function, direct-threaded through a 1024-entry table of
 * label addresses indexed by opcode bits 15..6, which is enough to decode
 * every Thumb format to its operation (only register numbers and immediates
 * are left to extract in the handler). N and Z live in one word (nz: the last
 * flag-setting result, N = bit 31, Z = nz == 0), which every Thumb
 * flag-setting instruction can use because N and Z always come from the same
 * 32-bit result.
 *
 * Rules for the firmware copies, which run this from SRAM on a Pico:
 *   - the dispatch table goes where THUMB_DATA says - a non-const static is
 *     NOT enough, GCC moves a table nothing writes to .rodata (flash);
 *   - no call to libgcc (no division, no __builtin_popcount, no switch that
 *     GCC could turn into a __gnu_thumb1_case_* helper on Cortex-M0+);
 *   - the only calls are the slow paths and the callbacks, taken for
 *     peripheral accesses and driver calls, i.e. almost never.
 */
#include "thumb_core.h"

#include <stddef.h>

static THUMB_HOT __attribute__((noinline)) uint32_t slow_read(thumb_cpu_t *cpu, uint32_t addr, uint32_t size)
{
    if (!cpu->periph_read)
        return 0;
    return cpu->periph_read(cpu, addr, size);
}

static THUMB_HOT __attribute__((noinline)) void slow_write(thumb_cpu_t *cpu, uint32_t addr, uint32_t val, uint32_t size)
{
    if (cpu->periph_write)
        cpu->periph_write(cpu, addr, val, size);
}

uint32_t THUMB_HOT thumb_run(thumb_cpu_t *cpu, uint32_t max)
{
    static THUMB_DATA void *tbl[1024] = {
        [0x000 ... 0x01F] = &&f1_lsl,     [0x020 ... 0x03F] = &&f1_lsr,
        [0x040 ... 0x05F] = &&f1_asr,
        [0x060 ... 0x067] = &&f2_add_reg, [0x068 ... 0x06F] = &&f2_sub_reg,
        [0x070 ... 0x077] = &&f2_add_imm, [0x078 ... 0x07F] = &&f2_sub_imm,
        [0x080 ... 0x09F] = &&f3_mov,     [0x0A0 ... 0x0BF] = &&f3_cmp,
        [0x0C0 ... 0x0DF] = &&f3_add,     [0x0E0 ... 0x0FF] = &&f3_sub,
        [0x100] = &&alu_and, [0x101] = &&alu_eor, [0x102] = &&alu_lsl, [0x103] = &&alu_lsr,
        [0x104] = &&alu_asr, [0x105] = &&alu_adc, [0x106] = &&alu_sbc, [0x107] = &&alu_ror,
        [0x108] = &&alu_tst, [0x109] = &&alu_neg, [0x10A] = &&alu_cmp, [0x10B] = &&alu_cmn,
        [0x10C] = &&alu_orr, [0x10D] = &&alu_mul, [0x10E] = &&alu_bic, [0x10F] = &&alu_mvn,
        [0x110 ... 0x113] = &&hi_add,     [0x114 ... 0x117] = &&hi_cmp,
        [0x118 ... 0x11B] = &&hi_mov,     [0x11C ... 0x11F] = &&hi_bx,
        [0x120 ... 0x13F] = &&f6_ldr_pc,
        [0x140 ... 0x147] = &&f7_str,     [0x148 ... 0x14F] = &&f8_strh,
        [0x150 ... 0x157] = &&f7_strb,    [0x158 ... 0x15F] = &&f8_ldsb,
        [0x160 ... 0x167] = &&f7_ldr,     [0x168 ... 0x16F] = &&f8_ldrh,
        [0x170 ... 0x177] = &&f7_ldrb,    [0x178 ... 0x17F] = &&f8_ldsh,
        [0x180 ... 0x19F] = &&f9_str,     [0x1A0 ... 0x1BF] = &&f9_ldr,
        [0x1C0 ... 0x1DF] = &&f9_strb,    [0x1E0 ... 0x1FF] = &&f9_ldrb,
        [0x200 ... 0x21F] = &&f10_strh,   [0x220 ... 0x23F] = &&f10_ldrh,
        [0x240 ... 0x25F] = &&f11_str,    [0x260 ... 0x27F] = &&f11_ldr,
        [0x280 ... 0x29F] = &&f12_add_pc, [0x2A0 ... 0x2BF] = &&f12_add_sp,
        [0x2C0 ... 0x2C3] = &&f13_add_sp, [0x2C4 ... 0x2CF] = &&undef,
        [0x2D0 ... 0x2D7] = &&f14_push,   [0x2D8 ... 0x2EF] = &&undef,
        [0x2F0 ... 0x2F7] = &&f14_pop,    [0x2F8 ... 0x2FF] = &&undef,
        [0x300 ... 0x31F] = &&f15_stm,    [0x320 ... 0x33F] = &&f15_ldm,
        [0x340 ... 0x343] = &&b_eq, [0x344 ... 0x347] = &&b_ne, [0x348 ... 0x34B] = &&b_cs,
        [0x34C ... 0x34F] = &&b_cc, [0x350 ... 0x353] = &&b_mi, [0x354 ... 0x357] = &&b_pl,
        [0x358 ... 0x35B] = &&b_vs, [0x35C ... 0x35F] = &&b_vc, [0x360 ... 0x363] = &&b_hi,
        [0x364 ... 0x367] = &&b_ls, [0x368 ... 0x36B] = &&b_ge, [0x36C ... 0x36F] = &&b_lt,
        [0x370 ... 0x373] = &&b_gt, [0x374 ... 0x377] = &&b_le, [0x378 ... 0x37B] = &&b_al,
        [0x37C ... 0x37F] = &&undef,      /* SWI */
        [0x380 ... 0x3BF] = &&f18_b,
        [0x3C0 ... 0x3DF] = &&f19_bl_hi,  [0x3E0 ... 0x3FF] = &&f19_bl_lo,
    };

    uint32_t *const R = cpu->r;
    const uint8_t *const rom = cpu->rom;
    uint8_t *const ram = cpu->ram;
    const uint32_t rom_size = cpu->rom_size, ram_size = cpu->ram_size;
    uint32_t pc = R[15], cnt = 0, op = 0;
    uint32_t nz = cpu->z ? 0 : (cpu->n ? 0x80000000u : 1u);
    uint32_t c = cpu->c, v = cpu->v;
    uintptr_t code;         /* host address of guest address 0 in the code region */
    uint32_t code_lo, code_len;

    cpu->status = THUMB_ENDED;

/* ---- helpers --------------------------------------------------------------------- */
#define NEXT do { op = *(const uint16_t *)(code + pc); pc += 2; cnt++; goto *tbl[op >> 6]; } while (0)

/* Branch: pc = target. The region check is one compare in the common case; the
   instruction budget is checked here because every loop contains a branch. */
#define JUMP(t_) do { pc = (t_); \
        if (pc - code_lo >= code_len) goto resolve; \
        if (cnt >= max) goto budget; \
        NEXT; } while (0)

#define RD(dst_, addr_, sz_, T_) do { uint32_t a_ = (addr_) & ~(uint32_t)((sz_) - 1); \
        uint32_t o_ = a_ - THUMB_RAM_BASE; \
        if (o_ < ram_size) dst_ = *(const T_ *)(ram + o_); \
        else if (a_ < rom_size) dst_ = *(const T_ *)(rom + a_); \
        else { dst_ = (T_)slow_read(cpu, a_, (sz_)); if (cpu->status) goto out; } } while (0)

#define WR(addr_, val_, sz_, T_) do { uint32_t a_ = (addr_) & ~(uint32_t)((sz_) - 1); \
        uint32_t o_ = a_ - THUMB_RAM_BASE; \
        if (o_ < ram_size) *(T_ *)(ram + o_) = (T_)(val_); \
        else { slow_write(cpu, a_, (val_), (sz_)); if (cpu->status) goto out; } } while (0)

/* flags of a + b + cin, and of a - b as a + ~b + 1 */
#define ADDF(r_, a_, b_) do { uint32_t x_ = (a_), y_ = (b_); r_ = x_ + y_; \
        c = r_ < x_; v = ((x_ ^ r_) & (y_ ^ r_)) >> 31; nz = r_; } while (0)
#define SUBF(r_, a_, b_) do { uint32_t x_ = (a_), y_ = (b_); r_ = x_ - y_; \
        c = x_ >= y_; v = ((x_ ^ y_) & (x_ ^ r_)) >> 31; nz = r_; } while (0)

#define RD3(sh_) ((op >> (sh_)) & 7u)
#define N_OF(x_) ((x_) >> 31)

    goto resolve;           /* enter: find the code region of the first pc */

/* ---- format 1: move shifted register ------------------------------------------- */
f1_lsl: { uint32_t s = (op >> 6) & 31, x = R[RD3(3)];
        if (s) { c = (x >> (32 - s)) & 1; x <<= s; }
        R[op & 7] = x; nz = x; NEXT; }
f1_lsr: { uint32_t s = (op >> 6) & 31, x = R[RD3(3)];
        if (s) { c = (x >> (s - 1)) & 1; x >>= s; } else { c = x >> 31; x = 0; }
        R[op & 7] = x; nz = x; NEXT; }
f1_asr: { uint32_t s = (op >> 6) & 31, x = R[RD3(3)];
        if (s) { c = (x >> (s - 1)) & 1; x = (uint32_t)((int32_t)x >> s); }
        else { c = x >> 31; x = c ? 0xFFFFFFFFu : 0; }
        R[op & 7] = x; nz = x; NEXT; }

/* ---- format 2: add/subtract ------------------------------------------------------ */
f2_add_reg: { uint32_t r; ADDF(r, R[RD3(3)], R[RD3(6)]); R[op & 7] = r; NEXT; }
f2_sub_reg: { uint32_t r; SUBF(r, R[RD3(3)], R[RD3(6)]); R[op & 7] = r; NEXT; }
f2_add_imm: { uint32_t r; ADDF(r, R[RD3(3)], RD3(6)); R[op & 7] = r; NEXT; }
f2_sub_imm: { uint32_t r; SUBF(r, R[RD3(3)], RD3(6)); R[op & 7] = r; NEXT; }

/* ---- format 3: move/compare/add/subtract immediate ---------------------------- */
f3_mov: { uint32_t x = op & 0xFF; R[RD3(8)] = x; nz = x; NEXT; }
f3_cmp: { uint32_t r; SUBF(r, R[RD3(8)], op & 0xFF); (void)r; NEXT; }
f3_add: { uint32_t r, d = RD3(8); ADDF(r, R[d], op & 0xFF); R[d] = r; NEXT; }
f3_sub: { uint32_t r, d = RD3(8); SUBF(r, R[d], op & 0xFF); R[d] = r; NEXT; }

/* ---- format 4: ALU operations ------------------------------------------------------ */
alu_and: { uint32_t x = R[op & 7] & R[RD3(3)]; R[op & 7] = x; nz = x; NEXT; }
alu_eor: { uint32_t x = R[op & 7] ^ R[RD3(3)]; R[op & 7] = x; nz = x; NEXT; }
alu_orr: { uint32_t x = R[op & 7] | R[RD3(3)]; R[op & 7] = x; nz = x; NEXT; }
alu_bic: { uint32_t x = R[op & 7] & ~R[RD3(3)]; R[op & 7] = x; nz = x; NEXT; }
alu_mvn: { uint32_t x = ~R[RD3(3)]; R[op & 7] = x; nz = x; NEXT; }
alu_mul: { uint32_t x = R[op & 7] * R[RD3(3)]; R[op & 7] = x; nz = x; NEXT; }
alu_tst: { nz = R[op & 7] & R[RD3(3)]; NEXT; }
alu_cmp: { uint32_t r; SUBF(r, R[op & 7], R[RD3(3)]); (void)r; NEXT; }
alu_cmn: { uint32_t r; ADDF(r, R[op & 7], R[RD3(3)]); (void)r; NEXT; }
alu_neg: { uint32_t r; SUBF(r, 0, R[RD3(3)]); R[op & 7] = r; NEXT; }
alu_adc: { uint32_t a = R[op & 7], b = R[RD3(3)], r = a + b + c;
        c = c ? (r <= a) : (r < a); v = ((a ^ r) & (b ^ r)) >> 31;
        R[op & 7] = r; nz = r; NEXT; }
alu_sbc: { uint32_t a = R[op & 7], b = ~R[RD3(3)], r = a + b + c;
        c = c ? (r <= a) : (r < a); v = ((a ^ r) & (b ^ r)) >> 31;
        R[op & 7] = r; nz = r; NEXT; }
/* Shift by register: Gopher2600 uses the whole 32-bit register as the amount. */
alu_lsl: { uint32_t s = R[RD3(3)], x = R[op & 7];
        if (s == 0) { }
        else if (s < 32) { c = (x >> (32 - s)) & 1; x <<= s; }
        else if (s == 32) { c = x & 1; x = 0; }
        else { c = 0; x = 0; }
        R[op & 7] = x; nz = x; NEXT; }
alu_lsr: { uint32_t s = R[RD3(3)], x = R[op & 7];
        if (s == 0) { }
        else if (s < 32) { c = (x >> (s - 1)) & 1; x >>= s; }
        else if (s == 32) { c = x >> 31; x = 0; }
        else { c = 0; x = 0; }
        R[op & 7] = x; nz = x; NEXT; }
alu_asr: { uint32_t s = R[RD3(3)], x = R[op & 7];
        if (s == 0) { }
        else if (s < 32) { c = (x >> (s - 1)) & 1; x = (uint32_t)((int32_t)x >> s); }
        else { c = x >> 31; x = c ? 0xFFFFFFFFu : 0; }
        R[op & 7] = x; nz = x; NEXT; }
/* ROR: amount&0xff == 0 - nothing; amount&0x1f == 0 - C = bit 31; otherwise
   Gopher2600 takes C from bit (amount-1), which is 0 once amount > 32, and
   rotates by amount mod 32. */
alu_ror: { uint32_t s = R[RD3(3)], x = R[op & 7];
        if ((s & 0xFF) == 0) { }
        else if ((s & 0x1F) == 0) { c = x >> 31; }
        else {
            c = (s - 1 < 32) ? (x >> (s - 1)) & 1 : 0;
            uint32_t k = s & 31;
            x = (x >> k) | (x << (32 - k));
        }
        R[op & 7] = x; nz = x; NEXT; }

/* ---- format 5: hi register operations / BX ----------------------------------- */
hi_add: { uint32_t d = (op & 7) | ((op >> 4) & 8), s = (op >> 3) & 15;
        uint32_t b = (s == 15) ? pc + 2 : R[s];
        if (d == 15) JUMP((pc + 2 + b) & ~1u);
        R[d] += b; NEXT; }
hi_cmp: { uint32_t d = (op & 7) | ((op >> 4) & 8), s = (op >> 3) & 15, r;
        uint32_t a = (d == 15) ? pc + 2 : R[d], b = (s == 15) ? pc + 2 : R[s];
        SUBF(r, a, b); (void)r; NEXT; }
hi_mov: { uint32_t d = (op & 7) | ((op >> 4) & 8), s = (op >> 3) & 15;
        uint32_t b = (s == 15) ? pc + 2 : R[s];
        if (d == 15) JUMP(b & ~1u);
        R[d] = b; NEXT; }
hi_bx: { uint32_t s = (op >> 3) & 15, x = (s == 15) ? pc + 2 : R[s];
        if (x & 1) JUMP(x & ~1u);
        /* ARM state: the end of the program, or a Harmony driver call */
        if (x == cpu->exit_addr) goto out;
        if (!cpu->driver || !cpu->driver(cpu, pc - 2)) { cpu->status = THUMB_ERR_DRIVER; goto fail; }
        if (cpu->status) goto out;
        /* the driver returns to the caller: Gopher2600 continues at LR as is */
        if (R[14] & 1) { cpu->status = THUMB_ERR_PC; goto fail; }
        JUMP(R[14]); }

/* ---- format 6: PC-relative load ---------------------------------------------------- */
f6_ldr_pc: { uint32_t x; RD(x, ((pc + 2) & ~3u) + ((op & 0xFF) << 2), 4, uint32_t); R[RD3(8)] = x; NEXT; }

/* ---- formats 7 and 8: register offset ---------------------------------------------- */
#define ROFS (R[RD3(3)] + R[RD3(6)])
f7_str:  { WR(ROFS, R[op & 7], 4, uint32_t); NEXT; }
f7_strb: { WR(ROFS, R[op & 7], 1, uint8_t); NEXT; }
f7_ldr:  { uint32_t x; RD(x, ROFS, 4, uint32_t); R[op & 7] = x; NEXT; }
f7_ldrb: { uint32_t x; RD(x, ROFS, 1, uint8_t); R[op & 7] = x; NEXT; }
f8_strh: { WR(ROFS, R[op & 7], 2, uint16_t); NEXT; }
f8_ldrh: { uint32_t x; RD(x, ROFS, 2, uint16_t); R[op & 7] = x; NEXT; }
f8_ldsb: { uint32_t x; RD(x, ROFS, 1, uint8_t); R[op & 7] = (uint32_t)(int32_t)(int8_t)x; NEXT; }
f8_ldsh: { uint32_t x; RD(x, ROFS, 2, uint16_t); R[op & 7] = (uint32_t)(int32_t)(int16_t)x; NEXT; }

/* ---- formats 9 and 10: immediate offset ------------------------------------------ */
#define IOFS(sh_) (R[RD3(3)] + (((op >> 6) & 31) << (sh_)))
f9_str:   { WR(IOFS(2), R[op & 7], 4, uint32_t); NEXT; }
f9_ldr:   { uint32_t x; RD(x, IOFS(2), 4, uint32_t); R[op & 7] = x; NEXT; }
f9_strb:  { WR(IOFS(0), R[op & 7], 1, uint8_t); NEXT; }
f9_ldrb:  { uint32_t x; RD(x, IOFS(0), 1, uint8_t); R[op & 7] = x; NEXT; }
f10_strh: { WR(IOFS(1), R[op & 7], 2, uint16_t); NEXT; }
f10_ldrh: { uint32_t x; RD(x, IOFS(1), 2, uint16_t); R[op & 7] = x; NEXT; }

/* ---- formats 11-13: SP-relative, load address, adjust SP ----------------------- */
f11_str:    { WR(R[13] + ((op & 0xFF) << 2), R[RD3(8)], 4, uint32_t); NEXT; }
f11_ldr:    { uint32_t x; RD(x, R[13] + ((op & 0xFF) << 2), 4, uint32_t); R[RD3(8)] = x; NEXT; }
f12_add_pc: { R[RD3(8)] = ((pc + 2) & ~3u) + ((op & 0xFF) << 2); NEXT; }
f12_add_sp: { R[RD3(8)] = R[13] + ((op & 0xFF) << 2); NEXT; }
f13_add_sp: { uint32_t imm = (op & 0x7F) << 2; if (op & 0x80) R[13] -= imm; else R[13] += imm; NEXT; }

/* ---- format 14: push/pop ------------------------------------------------------------ */
f14_push: { uint32_t n = (op >> 8) & 1, a, i;
        for (i = 0; i < 8; i++) n += (op >> i) & 1;
        a = R[13] - 4 * n;
        for (i = 0; i < 8; i++) if (op & (1u << i)) { WR(a, R[i], 4, uint32_t); a += 4; }
        if (op & 0x100) WR(a, R[14], 4, uint32_t);
        R[13] -= 4 * n; NEXT; }
f14_pop: { uint32_t a = R[13], i;
        for (i = 0; i < 8; i++) if (op & (1u << i)) { uint32_t x; RD(x, a, 4, uint32_t); R[i] = x; a += 4; }
        if (op & 0x100) { uint32_t x; RD(x, a, 4, uint32_t); R[13] = a + 4; JUMP(x & ~1u); }
        R[13] = a; NEXT; }

/* ---- format 15: multiple load/store ------------------------------------------------- */
f15_stm: { uint32_t b = RD3(8), a = R[b], i;
        for (i = 0; i < 8; i++) if (op & (1u << i)) { WR(a, R[i], 4, uint32_t); a += 4; }
        R[b] = a; NEXT; }
f15_ldm: { uint32_t b = RD3(8), a = R[b], i, wb = 1;
        for (i = 0; i < 8; i++) if (op & (1u << i)) {
            uint32_t x; RD(x, a, 4, uint32_t);
            if (i == b) wb = 0;
            R[i] = x; a += 4;
        }
        if (wb) R[b] = a;
        NEXT; }

/* ---- format 16: conditional branch (target = instruction + 4 + offset) ------ */
#define BCOND(lbl_, cond_) lbl_: if (cond_) JUMP(pc + 2 + (uint32_t)((int32_t)(op << 24) >> 23)); NEXT;
BCOND(b_eq, nz == 0)
BCOND(b_ne, nz != 0)
BCOND(b_cs, c)
BCOND(b_cc, !c)
BCOND(b_mi, N_OF(nz))
BCOND(b_pl, !N_OF(nz))
BCOND(b_vs, v)
BCOND(b_vc, !v)
BCOND(b_hi, c && nz != 0)
BCOND(b_ls, !c || nz == 0)
BCOND(b_ge, N_OF(nz) == v)
BCOND(b_lt, N_OF(nz) != v)
BCOND(b_gt, nz != 0 && N_OF(nz) == v)
BCOND(b_le, nz == 0 || N_OF(nz) != v)
BCOND(b_al, 1)

/* ---- formats 18 and 19: branch, branch with link -------------------------------- */
f18_b:     JUMP(pc + 2 + (uint32_t)((int32_t)(op << 21) >> 20));
f19_bl_hi: { R[14] = pc + 2 + (uint32_t)((int32_t)(op << 21) >> 9); NEXT; }
f19_bl_lo: { uint32_t t = R[14] + ((op & 0x7FF) << 1); R[14] = pc | 1; JUMP(t); }

undef:
    cpu->status = THUMB_ERR_UNDEFINED;
    goto fail;

budget:
    cpu->status = THUMB_ERR_BUDGET;
    goto fail;

resolve:
    /* pc left the current code region (or this is the first instruction) */
    if (pc & 1) { cpu->status = THUMB_ERR_PC; goto fail; }
    if (pc + 1 < rom_size) { code = (uintptr_t)rom; code_lo = 0; code_len = rom_size - 1; }
    else if (pc - THUMB_RAM_BASE + 1 < ram_size) {
        code = (uintptr_t)ram - THUMB_RAM_BASE; code_lo = THUMB_RAM_BASE; code_len = ram_size - 1;
    } else { cpu->status = THUMB_ERR_PC; goto fail; }
    if (cnt >= max) goto budget;
    NEXT;

fail:
    cpu->err_pc = pc - 2;
    cpu->err_op = op;
out:
    R[15] = pc;
    cpu->n = N_OF(nz);
    cpu->z = nz == 0;
    cpu->c = c;
    cpu->v = v;
    cpu->count = cnt;
    return cpu->status;
}
