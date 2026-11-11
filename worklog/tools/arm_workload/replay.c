/*
 * replay - run every call of an armstat recording (ARMREC01, see
 * ../armrec.py) through thumb_core and compare the result with Gopher2600's
 * bit for bit: r0-r14, the NZCV flags, the instruction count and every byte
 * of Harmony RAM. Driver calls and peripheral reads are answered from the
 * recording, and their order, addresses and arguments are checked too.
 *
 * usage: replay [-q] [-t] FILE.rec...
 *        replay -m PAGES.txt FILE.rec
 *   -q   one summary line per file
 *   -t   also time the interpreter on this host (not a Pico figure)
 *   -m   write the 256-byte ROM pages these calls need (see minimise())
 *
 * Build: make -C tools/arm_carts/thumb
 */
#include "thumb_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint32_t origin, size, writable;
} seg_t;

typedef struct {
    const uint8_t *ev;      /* next event, 6 x u32 each */
    uint32_t nev, used;
    int bad;
    char why[160];
} evstate_t;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t cb_read(thumb_cpu_t *cpu, uint32_t addr, uint32_t size)
{
    evstate_t *es = cpu->user;
    if (es->used >= es->nev) {
        snprintf(es->why, sizeof es->why, "unrecorded peripheral read %08x/%u", addr, size);
        cpu->status = THUMB_ERR_CALLBACK;
        return 0;
    }
    const uint8_t *e = es->ev + 24 * es->used++;
    if (rd32(e) != 2 || rd32(e + 4) != addr || rd32(e + 12) != size) {
        snprintf(es->why, sizeof es->why, "peripheral read %08x/%u, recording has event %u at %08x",
                 addr, size, rd32(e), rd32(e + 4));
        cpu->status = THUMB_ERR_CALLBACK;
        return 0;
    }
    return rd32(e + 8);
}

static int cb_driver(thumb_cpu_t *cpu, uint32_t addr)
{
    evstate_t *es = cpu->user;
    if (es->used >= es->nev) {
        snprintf(es->why, sizeof es->why, "unrecorded driver call at %08x", addr);
        cpu->status = THUMB_ERR_CALLBACK;
        return 1;
    }
    const uint8_t *e = es->ev + 24 * es->used++;
    if (rd32(e) != 1 || rd32(e + 4) != addr || rd32(e + 8) != cpu->r[2] || rd32(e + 12) != cpu->r[3]) {
        snprintf(es->why, sizeof es->why, "driver call %08x(r2=%08x r3=%08x), recording: event %u %08x(%08x %08x)",
                 addr, cpu->r[2], cpu->r[3], rd32(e), rd32(e + 4), rd32(e + 8), rd32(e + 12));
        cpu->status = THUMB_ERR_CALLBACK;
        return 1;
    }
    uint32_t sr = rd32(e + 16);
    if (sr & 1)
        cpu->r[(sr >> 8) & 15] = rd32(e + 20);
    return 1;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef struct {
    uint32_t index, frame;
    const uint8_t *regs_in, *flags_in, *ram_in, *ev, *regs_out, *flags_out, *ram_out;
    uint32_t nev, status;
    uint64_t count;
} call_t;

typedef struct {
    uint8_t *file;
    uint8_t *rom, *ram;
    uint32_t rom_size, ram_size, entry_lr;
    call_t *calls;
    unsigned ncalls;
} rec_t;

static int load_rec(const char *path, rec_t *r)
{
    memset(r, 0, sizeof *r);
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len);
    if (fread(b, 1, len, f) != (size_t)len) { perror(path); fclose(f); return 1; }
    fclose(f);
    if (len < 12 || memcmp(b, "ARMREC01", 8)) { fprintf(stderr, "%s: not ARMREC01\n", path); return 1; }

    long o = 8;
    uint32_t nseg = rd32(b + o); o += 4;
    seg_t seg[16];
    if (nseg > 16) { fprintf(stderr, "%s: %u segments\n", path, nseg); return 1; }
    for (uint32_t i = 0; i < nseg; i++, o += 12) {
        seg[i].origin = rd32(b + o); seg[i].size = rd32(b + o + 4); seg[i].writable = rd32(b + o + 8) & 1;
    }
    /* ROM: the read-only segments must tile [0, rom_size); RAM: the writable
       ones must tile [0x40000000, +ram_size) - true for DPC+ and every CDF
       version, and exactly what the interpreter's two-region map assumes. */
    for (uint32_t i = 0; i < nseg; i++) {
        if (!seg[i].writable) {
            if (seg[i].origin != r->rom_size) { fprintf(stderr, "%s: ROM segment %08x not contiguous\n", path, seg[i].origin); return 1; }
            r->rom_size += seg[i].size;
        } else {
            if (seg[i].origin != THUMB_RAM_BASE + r->ram_size) { fprintf(stderr, "%s: RAM segment %08x not contiguous\n", path, seg[i].origin); return 1; }
            r->ram_size += seg[i].size;
        }
    }
    r->rom = malloc((r->rom_size + 7) & ~3u);     /* malloc: 16-byte aligned */
    r->ram = malloc((r->ram_size + 7) & ~3u);
    memset(r->rom, 0, (r->rom_size + 7) & ~3u);
    memcpy(r->rom, b + o, r->rom_size);
    o += r->rom_size;
    r->entry_lr = rd32(b + o + 4);
    o += 12;
    r->file = b;

    unsigned cap = 0;
    while (o < len) {
        if (memcmp(b + o, "CALL", 4)) { fprintf(stderr, "%s: bad call marker at %ld\n", path, o); return 1; }
        if (r->ncalls == cap) { cap = cap ? 2 * cap : 64; r->calls = realloc(r->calls, cap * sizeof *r->calls); }
        call_t *c = &r->calls[r->ncalls++];
        const uint8_t *p = b + o + 4;
        c->index = rd32(p); c->frame = rd32(p + 4);
        c->regs_in = p + 8; c->flags_in = p + 72; c->ram_in = p + 76;
        p = c->ram_in + r->ram_size;
        c->nev = rd32(p);
        c->ev = p + 4;
        p = c->ev + 24 * c->nev;
        c->regs_out = p; c->flags_out = p + 64;
        c->count = rd32(p + 68) | (uint64_t)rd32(p + 72) << 32;
        c->status = rd32(p + 76);
        c->ram_out = p + 80;
        o = (c->ram_out + r->ram_size) - b;
    }
    return 0;
}

/* Run one call and compare. Returns "" when identical, else the first difference. */
static const char *run_call(rec_t *r, const call_t *c, double *seconds)
{
    static char why[240];
    thumb_cpu_t cpu;
    evstate_t es;
    memset(&cpu, 0, sizeof cpu);
    memset(&es, 0, sizeof es);
    es.ev = c->ev;
    es.nev = c->nev;
    for (int i = 0; i < 16; i++) cpu.r[i] = rd32(c->regs_in + 4 * i);
    uint32_t fl = rd32(c->flags_in);
    cpu.n = fl >> 31 & 1; cpu.z = fl >> 30 & 1; cpu.c = fl >> 29 & 1; cpu.v = fl >> 28 & 1;
    cpu.rom = r->rom; cpu.rom_size = r->rom_size;
    cpu.ram = r->ram; cpu.ram_size = r->ram_size;
    cpu.exit_addr = r->entry_lr & ~1u;
    cpu.periph_read = cb_read;
    cpu.driver = cb_driver;
    cpu.user = &es;
    memcpy(r->ram, c->ram_in, r->ram_size);

    double t0 = now();
    uint32_t st = thumb_run(&cpu, 50u * 1000 * 1000);
    if (seconds) *seconds += now() - t0;

    why[0] = 0;
    if (c->status != 0)
        snprintf(why, sizeof why, "Gopher2600 did not end this call normally (status %u) - skipped", c->status);
    else if (st != THUMB_ENDED)
        snprintf(why, sizeof why, "interpreter status %u at pc %08x op %04x%s%s", st, cpu.err_pc, cpu.err_op,
                 es.why[0] ? ": " : "", es.why);
    else if (cpu.count != c->count)
        snprintf(why, sizeof why, "instruction count %u, Gopher2600 %llu", cpu.count, (unsigned long long)c->count);
    else if (es.used != c->nev)
        snprintf(why, sizeof why, "used %u of %u recorded events", es.used, c->nev);
    else {
        for (int i = 0; i < 15 && !why[0]; i++)
            if (cpu.r[i] != rd32(c->regs_out + 4 * i))
                snprintf(why, sizeof why, "r%d = %08x, Gopher2600 %08x", i, cpu.r[i], rd32(c->regs_out + 4 * i));
        uint32_t flo = rd32(c->flags_out);
        uint32_t mine = cpu.n << 31 | cpu.z << 30 | cpu.c << 29 | cpu.v << 28;
        if (!why[0] && mine != (flo & 0xF0000000u))
            snprintf(why, sizeof why, "flags %08x, Gopher2600 %08x", mine, flo);
        for (uint32_t i = 0; i < r->ram_size && !why[0]; i++)
            if (r->ram[i] != c->ram_out[i])
                snprintf(why, sizeof why, "RAM %08x = %02x, Gopher2600 %02x", THUMB_RAM_BASE + i, r->ram[i], c->ram_out[i]);
    }
    return why;
}

static unsigned run_all(rec_t *r)
{
    unsigned ok = 0;
    for (unsigned i = 0; i < r->ncalls; i++)
        if (!run_call(r, &r->calls[i], NULL)[0]) ok++;
    return ok;
}

/* Greedy ROM minimisation: zero one 256-byte page at a time and keep it zeroed
   if every call still replays bit for bit. What remains is a ROM subset that
   reproduces these calls exactly - the part worth embedding in a benchmark. */
static int minimise(rec_t *r, const char *out)
{
    unsigned want = run_all(r);
    if (want != r->ncalls) { fprintf(stderr, "not every call replays; refusing to minimise\n"); return 1; }
    uint8_t save[256];
    unsigned pages = (r->rom_size + 255) / 256, kept = 0;
    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 1; }
    for (unsigned p = 0; p < pages; p++) {
        uint32_t n = r->rom_size - 256 * p < 256 ? r->rom_size - 256 * p : 256;
        memcpy(save, r->rom + 256 * p, n);
        memset(r->rom + 256 * p, 0, n);
        if (run_all(r) != want) {
            memcpy(r->rom + 256 * p, save, n);
            fprintf(f, "%u\n", p);
            kept++;
        }
    }
    fclose(f);
    printf("%u of %u ROM pages needed (%u bytes)\n", kept, pages, kept * 256);
    return 0;
}

static int replay_file(const char *path, int quiet, int timing)
{
    rec_t r;
    if (load_rec(path, &r)) return 1;
    unsigned nok = 0;
    unsigned long long instr = 0;
    double t_total = 0;
    for (unsigned i = 0; i < r.ncalls; i++) {
        const call_t *c = &r.calls[i];
        const char *why = run_call(&r, c, &t_total);
        if (!why[0]) {
            nok++;
            instr += c->count;
        } else if (!quiet)
            printf("  call#%u frame %u: %s\n", c->index, c->frame, why);
    }
    printf("%s: %u/%u calls identical, %llu instructions", path, nok, r.ncalls, instr);
    if (timing && t_total > 0)
        printf(", host %.1f MIPS", instr / t_total / 1e6);
    printf("\n");
    free(r.file); free(r.rom); free(r.ram); free(r.calls);
    return nok == r.ncalls ? 0 : 1;
}

int main(int argc, char **argv)
{
    int quiet = 0, timing = 0, rc = 0, files = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 2 < argc) {
            rec_t r;
            if (load_rec(argv[i + 2], &r)) return 1;
            return minimise(&r, argv[i + 1]);
        }
        if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-t")) timing = 1;
        else { rc |= replay_file(argv[i], quiet, timing); files++; }
    }
    if (!files) {
        fprintf(stderr, "usage: replay [-q] [-t] FILE.rec... | replay -m PAGES.txt FILE.rec\n");
        return 2;
    }
    return rc;
}
