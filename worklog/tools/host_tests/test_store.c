/*
 * Host tests for the results store (src/storage/results_store.c) and the virtual
 * FAT16 disk (src/usb/msc_disk.c).
 *
 * Flash is simulated in RAM with NOR semantics (erase -> 0xFF, program can only clear
 * bits). Power loss is simulated by aborting at the N-th erase/program operation, with
 * the interrupted operation leaving garbage (erase) or a partially programmed page.
 *
 * Checks:
 *   1. many runs of random sizes: every listed run reads back exactly, ring wraps,
 *      old runs are evicted, the table never exceeds STORE_MAX_RUNS;
 *   2. power loss at EVERY flash operation of a write, in several store states:
 *      after "reboot" every listed run is intact and the latest run is either the
 *      previous one or the new one;
 *   3. writes the 16 MB virtual disk image to $OUT_IMG for fsck/mount by run.sh.
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/flash.h"
#include "storage/flash_layout.h"
#include "storage/results_store.h"
#include "usb/msc_disk.h"

#include "boards/picobench.h"   // PICO_FLASH_SIZE_BYTES, as the SDK sees it

#define FLASH_BYTES (4u * 1024 * 1024)   // default simulated chip (Pico 2)
#define FLASH_MAX   (16u * 1024 * 1024)  // largest supported chip (WeAct RP2350B)

static uint8_t s_flash[FLASH_MAX];
uint8_t *g_host_flash = s_flash;
uint64_t g_host_time_us;
static uint8_t s_overlay[64 * 1024];

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// ---- Simulated flash with power-loss injection ----------------------------------------

static long s_ops;            // flash operations so far
static long s_fail_at = -1;   // abort at this operation (-1: never)
static jmp_buf s_power_loss;
static unsigned s_rng = 12345;

static unsigned rnd(void) {
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

static void maybe_lose_power(uint8_t *start, size_t len, const uint8_t *data) {
    if (s_fail_at < 0 || s_ops++ != s_fail_at) return;
    // Interrupted operation: an erase leaves random garbage in part of the range, a
    // program leaves a random prefix programmed.
    size_t k = rnd() % (len + 1);
    for (size_t i = 0; i < k; i++) start[i] = data ? (start[i] & data[i]) : (uint8_t)rnd();
    longjmp(s_power_loss, 1);
}

// The SDK hard_asserts every erase/program against PICO_FLASH_SIZE_BYTES (phase 2 bug:
// it was 2 MB while the WeAct results area is at 12 MB). Mirror that check here.
static void sdk_size_assert(uint32_t off, size_t count) {
    if (off + count > (uint64_t)PICO_FLASH_SIZE_BYTES) {
        printf("SDK hard_assert would fire: 0x%x + %zu > PICO_FLASH_SIZE_BYTES\n", off, count);
        abort();
    }
}

void flash_range_erase(uint32_t off, size_t count) {
    sdk_size_assert(off, count);
    if (off % FLASH_SECTOR_SIZE || count % FLASH_SECTOR_SIZE || off + count > FLASH_MAX) {
        printf("bad erase %u %zu\n", off, count);
        abort();
    }
    maybe_lose_power(s_flash + off, count, NULL);
    memset(s_flash + off, 0xFF, count);
}

void flash_range_program(uint32_t off, const uint8_t *data, size_t count) {
    sdk_size_assert(off, count);
    if (off % FLASH_PAGE_SIZE || count % FLASH_PAGE_SIZE || off + count > FLASH_MAX) {
        printf("bad program %u %zu\n", off, count);
        abort();
    }
    maybe_lose_power(s_flash + off, count, data);
    for (size_t i = 0; i < count; i++) s_flash[off + i] &= data[i];   // NOR: 1 -> 0 only
}

// ---- Layout stub (the real one reads the JEDEC ID) ---------------------------------------

static flash_layout_t s_layout;

const flash_layout_t *flash_layout(void) { return &s_layout; }

static void layout_init(uint32_t flash_bytes) {
    s_layout.flash_size = flash_bytes;
    s_layout.size_detected = true;
    s_layout.results_size = flash_bytes >= 16u * 1024 * 1024 ? 4u * 1024 * 1024 : 1024u * 1024;
    s_layout.results_base = flash_bytes - s_layout.results_size;
    s_layout.scratch_base = s_layout.results_base - FLASH_SCRATCH_BYTES;
    s_layout.fw_end = 128u * 1024;
    s_layout.valid = true;
}

// ---- Deterministic file generators ------------------------------------------------------

typedef struct {
    uint32_t sizes[3];
    uint32_t seed;
} run_spec_t;

static run_spec_t s_spec;                 // spec of the run being written
static run_spec_t s_specs[1000];          // by run number

static void gen(sink_t *o, unsigned file) {
    char line[64];
    uint32_t left = s_spec.sizes[file];
    unsigned n = 0;
    while (left) {
        int len = snprintf(line, sizeof(line), "seed %08x file %u line %06u\n", s_spec.seed, file, n++);
        uint32_t k = (uint32_t)len < left ? (uint32_t)len : left;
        o->write(o, line, k);
        left -= k;
    }
}
static void gen0(sink_t *o) { gen(o, 0); }
static void gen1(sink_t *o) { gen(o, 1); }
static void gen2(sink_t *o) { gen(o, 2); }

static const store_file_spec_t s_files[] = {
    {"REPORT.TXT", gen0}, {"RESULTS.CSV", gen1}, {"ERRORS.TXT", gen2},
};

typedef struct {
    sink_t base;
    uint8_t buf[512 * 1024];
    uint32_t len;
} mem_sink_t;

static void mem_write(sink_t *s, const char *b, size_t n) {
    mem_sink_t *m = (mem_sink_t *)s;
    memcpy(m->buf + m->len, b, n);
    m->len += (uint32_t)n;
}

static mem_sink_t s_expect;

// Every listed run must match its generator output byte for byte.
static int verify_all_runs(void) {
    int bad = 0;
    CHECK(store_invalid_runs() == 0);
    CHECK(store_run_count() <= STORE_MAX_RUNS);
    for (unsigned i = 0; i < store_run_count(); i++) {
        const store_run_t *r = store_run(i);
        s_spec = s_specs[r->run_no];
        CHECK(r->nfiles == 3);
        for (unsigned f = 0; f < r->nfiles && f < 3; f++) {
            s_expect.base.write = mem_write;
            s_expect.len = 0;
            s_files[f].fn(&s_expect.base);
            const store_file_t *sf = &r->files[f];
            if (sf->size != s_expect.len || memcmp(store_file_data(r, sf), s_expect.buf, sf->size) != 0) {
                printf("run %u file %s mismatch\n", r->run_no, sf->name);
                bad++;
            }
        }
        if (i > 0) CHECK(store_run(i - 1)->run_no < r->run_no);
    }
    failures += bad;
    return bad;
}

static void new_spec(uint32_t run_no, uint32_t max_file) {
    s_spec.seed = rnd();
    for (int f = 0; f < 3; f++) s_spec.sizes[f] = rnd() % max_file;
    s_specs[run_no] = s_spec;
}

// ---- Tests -----------------------------------------------------------------------------

static void test_many_runs(void) {
    memset(s_flash, 0xFF, FLASH_BYTES);
    store_mount();
    CHECK(store_run_count() == 0 && store_next_run_no() == 1);
    for (unsigned i = 0; i < 60; i++) {
        uint32_t run_no = store_next_run_no();
        new_spec(run_no, i % 7 == 0 ? 200000 : 40000);   // some runs fill a good part of the ring
        CHECK(store_write_run(s_files, 3, 0xABC, run_no, STORE_RUN_PASS));
        store_mount();                                    // "reboot"
        CHECK(store_latest() && store_latest()->run_no == run_no);
        verify_all_runs();
    }
    printf("many runs: %u runs kept, next run %u, dir seq %u\n", store_run_count(),
           store_next_run_no(), store_dir_seq());
}

// Snapshot/restore only the results area: nothing else is ever written.
static void snap(uint8_t *dst) { memcpy(dst, s_flash + s_layout.results_base, s_layout.results_size); }
static void restore(const uint8_t *src) { memcpy(s_flash + s_layout.results_base, src, s_layout.results_size); }

static void test_power_loss(void) {
    static uint8_t before[1024 * 1024];
    unsigned trials = 0;
    for (unsigned scenario = 0; scenario < 12; scenario++) {
        // Advance the store to a new state (different ring positions, wrap, eviction).
        uint32_t run_no = store_next_run_no();
        new_spec(run_no, scenario % 3 == 0 ? 250000 : 30000);
        snap(before);

        // Count the flash operations of an uninterrupted write.
        s_ops = 0;
        s_fail_at = 1L << 40;
        store_mount();
        CHECK(store_write_run(s_files, 3, 0xABC, run_no, 0));
        long total_ops = s_ops;

        for (long k = 0; k < total_ops; k++) {
            restore(before);
            store_mount();
            const store_run_t *old_latest = store_latest();
            uint32_t old_no = old_latest ? old_latest->run_no : 0;
            s_ops = 0;
            s_fail_at = k;
            s_spec = s_specs[run_no];     // verify_all_runs() changes the generator spec
            if (setjmp(s_power_loss) == 0) {
                store_write_run(s_files, 3, 0xABC, run_no, 0);
                printf("power loss at op %ld did not trigger\n", k);
                failures++;
            }
            s_fail_at = -1;
            store_mount();                 // reboot after power loss
            verify_all_runs();
            uint32_t now = store_latest() ? store_latest()->run_no : 0;
            if (now != old_no && now != run_no) {
                printf("scenario %u op %ld: latest %u, expected %u or %u\n", scenario, k, now, old_no, run_no);
                failures++;
            }
            trials++;
        }
        // Leave the store with the run committed for the next scenario.
        restore(before);
        s_fail_at = -1;
        store_mount();
        s_spec = s_specs[run_no];
        CHECK(store_write_run(s_files, 3, 0xABC, run_no, 0));
        store_mount();
        verify_all_runs();
    }
    printf("power loss: %u interrupted writes checked\n", trials);
}

extern int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
extern int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
extern void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size);

static const char s_status[] = "status text\r\n";

// FAT16 geometry of the virtual disk (must match msc_disk.c).
#define ROOT_START 257u
#define DATA_START 289u

// Find a directory entry by 8.3 name in `nsec` sectors starting at `lba`; returns
// its LBA and byte offset, or false.
static bool find_entry(uint32_t lba, uint32_t nsec, const char name83[11], uint32_t *elba, uint32_t *eoff,
                       uint16_t *cluster) {
    uint8_t b[512];
    for (uint32_t s = 0; s < nsec; s++) {
        tud_msc_read10_cb(0, lba + s, 0, b, 512);
        for (uint32_t k = 0; k < 512; k += 32) {
            if (b[k] == 0) return false;
            if (b[k] != 0xE5 && memcmp(b + k, name83, 11) == 0) {
                *elba = lba + s;
                *eoff = k;
                *cluster = (uint16_t)(b[k + 26] | (b[k + 27] << 8));
                return true;
            }
        }
    }
    return false;
}

// Mark an entry deleted the way a host does: read-modify-write of the sector.
static void host_delete(uint32_t elba, uint32_t eoff) {
    uint8_t b[512];
    tud_msc_read10_cb(0, elba, 0, b, 512);
    b[eoff] = 0xE5;
    tud_msc_write10_cb(0, elba, 0, b, 512);
}

static bool store_has(uint32_t run_no) {
    for (unsigned i = 0; i < store_run_count(); i++)
        if (store_run(i)->run_no == run_no) return true;
    return false;
}

static void test_delete_via_disk(void) {
    store_mount();
    unsigned before = store_run_count();
    CHECK(before >= 3);
    uint32_t victim = store_run(1)->run_no;
    msc_disk_build(s_overlay, sizeof(s_overlay), s_status, sizeof(s_status) - 1);

    uint32_t elba, eoff;
    uint16_t runs_cl, run_cl;
    CHECK(find_entry(ROOT_START, 32, "RUNS       ", &elba, &eoff, &runs_cl));
    char name[12];
    snprintf(name, sizeof(name), "RUN%04u    ", victim);
    CHECK(find_entry(DATA_START + runs_cl - 2u, 2, name, &elba, &eoff, &run_cl));

    host_delete(elba, eoff);
    CHECK(!msc_disk_task());               // host not quiet yet: nothing applied
    CHECK(store_has(victim));
    g_host_time_us += 1000000;
    CHECK(msc_disk_task());                // applied
    CHECK(!store_has(victim) && store_run_count() == before - 1);
    store_mount();                         // survives "reboot"
    CHECK(!store_has(victim) && store_run_count() == before - 1);
    verify_all_runs();

    // The host still sees its own view (entry deleted) during the session.
    uint32_t l2, o2; uint16_t c2;
    CHECK(!find_entry(DATA_START + runs_cl - 2u, 2, name, &l2, &o2, &c2));

    // Deleting RUNS itself deletes every run; the last tested build is remembered.
    uint32_t last_build = store_last_build_id();
    CHECK(find_entry(ROOT_START, 32, "RUNS       ", &elba, &eoff, &runs_cl));
    host_delete(elba, eoff);
    g_host_time_us += 1000000;
    CHECK(msc_disk_task());
    store_mount();
    CHECK(store_run_count() == 0);
    CHECK(store_last_build_id() == last_build && last_build != 0);
    printf("delete via disk: RUN%04u, then all runs, deleted; last build %08x kept\n", victim, last_build);
}

static void write_disk_image(const char *path) {
    store_mount();
    msc_disk_build(s_overlay, sizeof(s_overlay), s_status, sizeof(s_status) - 1);
    uint32_t blocks;
    uint16_t bs;
    tud_msc_capacity_cb(0, &blocks, &bs);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); failures++; return; }
    static uint8_t buf[4096];
    for (uint32_t lba = 0; lba < blocks; lba += 8) {
        tud_msc_read10_cb(0, lba, 0, buf, sizeof(buf));   // 8 sectors per call
        fwrite(buf, 1, sizeof(buf), f);
    }
    fclose(f);
    printf("disk image: %s (%u x %u bytes), %u runs\n", path, blocks, bs, store_run_count());

    // Expected content of the latest run's files, for comparison with the mounted image.
    const store_run_t *r = store_latest();
    for (unsigned i = 0; r && i < r->nfiles; i++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s.expect.%s", path, r->files[i].name);
        FILE *e = fopen(p, "wb");
        if (!e) { perror(p); failures++; continue; }
        fwrite(store_file_data(r, &r->files[i]), 1, r->files[i].size, e);
        fclose(e);
    }
}

// WeAct RP2350B: 16 MB flash, results area at 12 MB. Catches size-limit mistakes such
// as the SDK hard_assert on PICO_FLASH_SIZE_BYTES.
static void test_16mb_board(void) {
    layout_init(FLASH_MAX);
    memset(s_flash, 0xFF, FLASH_MAX);
    store_mount();
    for (unsigned i = 0; i < 20; i++) {
        uint32_t run_no = store_next_run_no();
        new_spec(run_no, 300000);
        CHECK(store_write_run(s_files, 3, 0xDEF, run_no, 0));
        store_mount();
        verify_all_runs();
    }
    printf("16 MB board: results area at 0x%06x, %u runs kept\n", s_layout.results_base, store_run_count());
}

// Erasing the store must forget every run, restart numbering at RUN0001 and clear the
// remembered build id (so the next boot runs the tests again).
static void test_erase_all(void) {
    layout_init(FLASH_BYTES);
    memset(s_flash, 0xFF, FLASH_BYTES);
    store_mount();
    for (unsigned i = 0; i < 3; i++) {
        uint32_t run_no = store_next_run_no();
        new_spec(run_no, 20000);
        CHECK(store_write_run(s_files, 3, 0xABC, run_no, 0));
    }
    store_mount();
    CHECK(store_run_count() == 3);
    CHECK(store_next_run_no() == 4);
    CHECK(store_last_build_id() == 0xABC);

    CHECK(store_erase_all());
    CHECK(store_run_count() == 0);
    CHECK(store_next_run_no() == 1);
    CHECK(store_last_build_id() == 0);
    store_mount();                       // survives a "reboot"
    CHECK(store_run_count() == 0 && store_next_run_no() == 1);

    uint32_t run_no = store_next_run_no();
    new_spec(run_no, 20000);
    CHECK(store_write_run(s_files, 3, 0xABC, run_no, 0));
    store_mount();
    CHECK(store_latest() && store_latest()->run_no == 1);
    verify_all_runs();
    printf("erase all: numbering restarted at RUN%04u\n", store_latest()->run_no);
}

int main(void) {
    test_16mb_board();
    test_erase_all();
    layout_init(FLASH_BYTES);
    test_many_runs();
    test_power_loss();
    const char *img = getenv("OUT_IMG");
    if (img) write_disk_image(img);
    test_delete_via_disk();
    printf("%s (%d failure(s))\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
