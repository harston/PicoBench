// On the board the event callbacks run inside the timed region, so they live
// in SRAM with the interpreter. The host tests build this file without the SDK.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define THUMB_HOT __not_in_flash("awl")
#endif

#include "tests/arm/awl.h"

#include <string.h>

#include "results/crc32.h"

extern const uint32_t g_arm_workload[];
extern const uint32_t g_arm_workload_words;

#define AWL_MAGIC 0x314C5741u   // "AWL1"

unsigned awl_ntitles(void) {
    if (g_arm_workload_words < 2 || g_arm_workload[0] != AWL_MAGIC) return 0;
    return g_arm_workload[1];
}

uint32_t awl_id(void) {
    if (!awl_ntitles()) return 0;
    return crc32_update(0, g_arm_workload, g_arm_workload_words * 4u);
}

bool awl_title(unsigned i, awl_title_t *t) {
    if (i >= awl_ntitles()) return false;
    const uint32_t *p = g_arm_workload + g_arm_workload[2 + i] / 4u;
    memcpy(t->name, p, 16);
    t->name[16] = 0;
    memcpy(t->format, p + 4, 8);
    t->format[8] = 0;
    p += 6;
    t->need_mips_x10 = p[0];
    t->rom_size = p[1];
    t->ram_size = p[2];
    t->exit_addr = p[3];
    t->npages = p[4];
    t->pages = p + 5;
    p = t->pages + t->npages * 65u;
    t->ncalls = p[0];
    t->calls = p + 1;
    return true;
}

void awl_load_rom(const awl_title_t *t, uint8_t *rom) {
    memset(rom, 0, t->rom_size);
    for (uint32_t i = 0; i < t->npages; i++) {
        const uint32_t *pg = t->pages + i * 65u;
        uint32_t off = pg[0] * 256u;
        uint32_t n = t->rom_size - off < 256u ? t->rom_size - off : 256u;
        memcpy(rom + off, pg + 1, n);
    }
}

// ---- one call record -------------------------------------------------------------

typedef struct {
    const uint32_t *regs_in;
    uint32_t flags_in;
    const uint32_t *patches;    // npatch, then records
    const uint32_t *events;     // nev x 6
    uint32_t nev;
    const uint32_t *regs_out;   // 15 words, then flags, count, crc
    const uint32_t *next;
} awl_call_t;

static const uint32_t *parse_call(const uint32_t *p, awl_call_t *c) {
    c->regs_in = p;
    c->flags_in = p[16];
    p += 17;
    c->patches = p;
    uint32_t np = *p++;
    for (uint32_t i = 0; i < np; i++) {
        uint32_t len = p[1];
        p += 2 + (len + 3u) / 4u;
    }
    c->nev = *p++;
    c->events = p;
    p += 6u * c->nev;
    c->regs_out = p;
    c->next = p + 18;
    return c->next;
}

static void apply_patches(const uint32_t *p, uint8_t *ram, uint32_t ram_size) {
    uint32_t np = *p++;
    for (uint32_t i = 0; i < np; i++) {
        uint32_t off = p[0], len = p[1];
        if (off <= ram_size && len <= ram_size - off) memcpy(ram + off, p + 2, len);
        p += 2 + (len + 3u) / 4u;
    }
}

// ---- events: driver calls and peripheral reads, answered from the recording --------

typedef struct {
    const uint32_t *ev;
    uint32_t nev, used;
    bool bad;
} awl_events_t;

static THUMB_HOT uint32_t ev_read(thumb_cpu_t *cpu, uint32_t addr, uint32_t size) {
    awl_events_t *e = cpu->user;
    if (!e || e->used >= e->nev) { if (e) e->bad = true; return 0; }
    const uint32_t *x = e->ev + 6u * e->used++;
    if (x[0] != 2 || x[1] != addr || x[3] != size) e->bad = true;
    return x[2];
}

static THUMB_HOT int ev_driver(thumb_cpu_t *cpu, uint32_t addr) {
    awl_events_t *e = cpu->user;
    if (!e || e->used >= e->nev) { if (e) e->bad = true; return 1; }
    const uint32_t *x = e->ev + 6u * e->used++;
    if (x[0] != 1 || x[1] != addr || x[2] != cpu->r[2] || x[3] != cpu->r[3]) e->bad = true;
    if (x[4] & 1) cpu->r[(x[4] >> 8) & 15] = x[5];
    return 1;
}

static void setup_cpu(const awl_title_t *t, const uint8_t *rom, uint8_t *ram, const awl_call_t *c,
                      thumb_cpu_t *cpu) {
    memset(cpu, 0, sizeof *cpu);
    for (int i = 0; i < 16; i++) cpu->r[i] = c->regs_in[i];
    cpu->n = c->flags_in >> 31 & 1;
    cpu->z = c->flags_in >> 30 & 1;
    cpu->c = c->flags_in >> 29 & 1;
    cpu->v = c->flags_in >> 28 & 1;
    cpu->rom = rom;
    cpu->rom_size = t->rom_size;
    cpu->ram = ram;
    cpu->ram_size = t->ram_size;
    cpu->exit_addr = t->exit_addr;
    cpu->periph_read = ev_read;
    cpu->driver = ev_driver;
}

// 0 if the result matches, else AWL_BAD_* with expected/actual filled in.
static uint32_t check(const awl_title_t *t, const uint8_t *ram, const awl_call_t *c,
                      const thumb_cpu_t *cpu, uint32_t status, const awl_events_t *ev,
                      uint32_t *exp, uint32_t *act) {
    if (status != THUMB_ENDED) { *exp = THUMB_ENDED; *act = status; return AWL_BAD_STATUS; }
    if (ev->bad || ev->used != ev->nev) { *exp = ev->nev; *act = ev->used; return AWL_BAD_EVENTS; }
    if (cpu->count != c->regs_out[16]) { *exp = c->regs_out[16]; *act = cpu->count; return AWL_BAD_COUNT; }
    for (int i = 0; i < 15; i++)
        if (cpu->r[i] != c->regs_out[i]) { *exp = c->regs_out[i]; *act = cpu->r[i]; return AWL_BAD_REG; }
    uint32_t fl = cpu->n << 31 | cpu->z << 30 | cpu->c << 29 | cpu->v << 28;
    if (fl != (c->regs_out[15] & 0xF0000000u)) { *exp = c->regs_out[15]; *act = fl; return AWL_BAD_FLAGS; }
    uint32_t crc = crc32_update(0, ram, t->ram_size);
    if (crc != c->regs_out[17]) { *exp = c->regs_out[17]; *act = crc; return AWL_BAD_RAM; }
    return 0;
}

void awl_run(const awl_title_t *t, const uint8_t *rom, uint8_t *ram, awl_run_fn run, awl_result_t *res) {
    memset(res, 0, sizeof *res);
    res->first_bad = -1;
    memset(ram, 0, t->ram_size);
    const uint32_t *p = t->calls;
    for (uint32_t k = 0; k < t->ncalls; k++) {
        awl_call_t c;
        p = parse_call(p, &c);
        apply_patches(c.patches, ram, t->ram_size);
        thumb_cpu_t cpu;
        setup_cpu(t, rom, ram, &c, &cpu);
        awl_events_t ev = {c.events, c.nev, 0, false};
        cpu.user = &ev;
        uint32_t cycles = 0;
        uint32_t status = run(&cpu, 10u * 1000u * 1000u, &cycles);
        res->calls++;
        res->instructions += cpu.count;
        res->cycles += cycles;
        if (cycles > res->max_call_cycles) res->max_call_cycles = cycles;
        uint32_t exp = 0, act = 0, what = check(t, ram, &c, &cpu, status, &ev, &exp, &act);
        if (what) {
            if (res->first_bad < 0) {
                res->first_bad = (int)k;
                res->bad_what = what;
                res->bad_expected = exp;
                res->bad_actual = act;
            }
            res->bad++;
        }
    }
}

static uint32_t plain_run(thumb_cpu_t *cpu, uint32_t max, uint32_t *cycles) {
    (void)cycles;
    return thumb_run(cpu, max);
}

bool awl_prepare_call(const awl_title_t *t, const uint8_t *rom, uint8_t *ram, unsigned k,
                      thumb_cpu_t *cpu, uint32_t *instructions, uint32_t *events) {
    if (k >= t->ncalls) return false;
    memset(ram, 0, t->ram_size);
    const uint32_t *p = t->calls;
    for (unsigned i = 0; i <= k; i++) {
        awl_call_t c;
        p = parse_call(p, &c);
        apply_patches(c.patches, ram, t->ram_size);
        thumb_cpu_t tmp;
        setup_cpu(t, rom, ram, &c, &tmp);
        if (i == k) {
            *cpu = tmp;
            cpu->user = NULL;
            *instructions = c.regs_out[16];
            *events = c.nev;
            return true;
        }
        awl_events_t ev = {c.events, c.nev, 0, false};
        tmp.user = &ev;
        uint32_t cy = 0, exp, act;
        uint32_t status = plain_run(&tmp, 10u * 1000u * 1000u, &cy);
        if (check(t, ram, &c, &tmp, status, &ev, &exp, &act)) return false;
    }
    return false;
}
