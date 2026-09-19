#include "storage/results_store.h"

#include <string.h>

#include "core/guard.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "results/crc32.h"
#include "storage/flash_layout.h"

#define DIR_MAGIC     0x50424452u   // "PBDR"
#define DIR_VERSION   1u
#define COMMIT_MAGIC  0x434F4D54u   // "COMT"
#define COMMIT_OFFSET (FLASH_SECTOR_SIZE - 4u)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t next_run_no;
    uint32_t nruns;
    uint32_t last_build_id;             // build that last committed a run (0 = unknown)
    uint32_t reserved[2];
    store_run_t runs[STORE_MAX_RUNS];   // oldest first
    uint32_t crc;                        // over everything above
} store_dir_t;

// The commit word lives alone in the last page of the directory sector.
_Static_assert(sizeof(store_dir_t) <= FLASH_SECTOR_SIZE - FLASH_PAGE_SIZE,
               "directory must leave the last page free for the COMMITTED word");

static store_dir_t s_dir;          // current directory (RAM copy)
static int s_dir_slot = -1;        // slot holding s_dir in flash, -1 = none
static uint32_t s_invalid_runs;
static const char *s_last_error = "";
static uint8_t s_page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

// ---- Geometry ------------------------------------------------------------------------

static uint32_t dir_offset(int slot) {
    return flash_layout()->results_base + (uint32_t)slot * FLASH_SECTOR_SIZE;
}

static uint32_t data_base(void) {
    return flash_layout()->results_base + 2u * FLASH_SECTOR_SIZE;
}

static uint32_t data_size(void) {
    return flash_layout()->results_size - 2u * FLASH_SECTOR_SIZE;
}

static uint32_t align_sector(uint32_t v) {
    return (v + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
}

// ---- Raw flash operations (IRQs off: no flash-resident handler may run) -----------------

static void flash_erase_sector(uint32_t off) {
    guard_keepalive();   // ~50 ms per sector; a large run erases hundreds of them
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);
}

static void flash_program_page(uint32_t off, const uint8_t *data) {
    uint32_t irq = save_and_disable_interrupts();
    flash_range_program(off, data, FLASH_PAGE_SIZE);
    restore_interrupts(irq);
}

// ---- Directory -------------------------------------------------------------------------

static uint32_t dir_crc(const store_dir_t *d) {
    return crc32_update(0, d, offsetof(store_dir_t, crc));
}

static bool dir_valid(const store_dir_t *d) {
    const uint32_t *commit = (const uint32_t *)((const uint8_t *)d + COMMIT_OFFSET);
    return d->magic == DIR_MAGIC && d->version == DIR_VERSION &&
           d->nruns <= STORE_MAX_RUNS && d->crc == dir_crc(d) && *commit == COMMIT_MAGIC;
}

// Writes s_dir (seq incremented) to the slot not holding the current directory.
static bool dir_commit(void) {
    int slot = s_dir_slot == 0 ? 1 : 0;
    uint32_t off = dir_offset(slot);
    s_dir.magic = DIR_MAGIC;
    s_dir.version = DIR_VERSION;
    s_dir.seq++;
    s_dir.crc = dir_crc(&s_dir);

    flash_erase_sector(off);
    const uint8_t *src = (const uint8_t *)&s_dir;
    for (uint32_t p = 0; p < sizeof(store_dir_t); p += FLASH_PAGE_SIZE) {
        uint32_t n = sizeof(store_dir_t) - p;
        if (n > FLASH_PAGE_SIZE) n = FLASH_PAGE_SIZE;
        memset(s_page, 0xFF, sizeof(s_page));
        memcpy(s_page, src + p, n);
        flash_program_page(off + p, s_page);
    }
    if (memcmp(flash_xip_ptr(off), &s_dir, sizeof(store_dir_t)) != 0) return false;

    // Last step: the COMMITTED word. Until it is programmed, this slot is invalid and
    // the previous directory in the other slot stays authoritative.
    memset(s_page, 0xFF, sizeof(s_page));
    uint32_t commit = COMMIT_MAGIC;
    memcpy(s_page + FLASH_PAGE_SIZE - 4u, &commit, 4);
    flash_program_page(off + FLASH_SECTOR_SIZE - FLASH_PAGE_SIZE, s_page);

    if (!dir_valid((const store_dir_t *)flash_xip_ptr(off))) return false;
    s_dir_slot = slot;
    return true;
}

static bool run_crc_ok(const store_run_t *r) {
    if (r->blob_off + r->blob_len > data_size() || r->nfiles > STORE_MAX_FILES) return false;
    return crc32_update(0, flash_xip_ptr(data_base() + r->blob_off), r->blob_len) == r->blob_crc;
}

void store_mount(void) {
    memset(&s_dir, 0, sizeof(s_dir));
    s_dir.next_run_no = 1;
    s_dir_slot = -1;
    s_invalid_runs = 0;
    if (!flash_layout()->valid) return;

    const store_dir_t *a = (const store_dir_t *)flash_xip_ptr(dir_offset(0));
    const store_dir_t *b = (const store_dir_t *)flash_xip_ptr(dir_offset(1));
    bool va = dir_valid(a), vb = dir_valid(b);
    const store_dir_t *best = NULL;
    if (va && vb) {
        best = a->seq >= b->seq ? a : b;
    } else if (va) {
        best = a;
    } else if (vb) {
        best = b;
    }
    if (!best) return;

    memcpy(&s_dir, best, sizeof(s_dir));
    s_dir_slot = best == a ? 0 : 1;

    // Hide runs whose data do not verify (should never happen; reported in HARDWARE.TXT).
    uint32_t w = 0;
    for (uint32_t i = 0; i < s_dir.nruns; i++) {
        if (run_crc_ok(&s_dir.runs[i])) {
            s_dir.runs[w++] = s_dir.runs[i];
        } else {
            s_invalid_runs++;
        }
    }
    s_dir.nruns = w;
}

static void drop_run(unsigned i) {
    memmove(&s_dir.runs[i], &s_dir.runs[i + 1], (s_dir.nruns - i - 1) * sizeof(store_run_t));
    s_dir.nruns--;
}

unsigned store_run_count(void) { return s_dir.nruns; }
const store_run_t *store_run(unsigned i) { return i < s_dir.nruns ? &s_dir.runs[i] : NULL; }
const store_run_t *store_latest(void) { return s_dir.nruns ? &s_dir.runs[s_dir.nruns - 1] : NULL; }
uint32_t store_next_run_no(void) { return s_dir.next_run_no; }
uint32_t store_dir_seq(void) { return s_dir.seq; }
uint32_t store_invalid_runs(void) { return s_invalid_runs; }
const char *store_last_error(void) { return s_last_error; }

uint32_t store_last_build_id(void) {
    if (s_dir.last_build_id) return s_dir.last_build_id;
    const store_run_t *r = store_latest();   // directories written before the field existed
    return r ? r->build_id : 0;
}

bool store_delete_runs(const uint32_t *run_nos, unsigned n) {
    bool changed = false;
    for (unsigned k = 0; k < n; k++) {
        for (unsigned i = 0; i < s_dir.nruns; i++) {
            if (s_dir.runs[i].run_no == run_nos[k]) {
                drop_run(i);
                changed = true;
                break;
            }
        }
    }
    if (!changed) return true;
    if (!dir_commit()) {
        s_last_error = "directory commit failed while deleting runs";
        return false;
    }
    return true;
}

const uint8_t *store_file_data(const store_run_t *run, const store_file_t *f) {
    return flash_xip_ptr(data_base() + run->blob_off + f->offset);
}

// ---- Sinks ------------------------------------------------------------------------------

typedef struct {
    sink_t base;
    uint32_t count;
} count_sink_t;

static void count_write(sink_t *s, const char *buf, size_t n) {
    (void)buf;
    ((count_sink_t *)s)->count += (uint32_t)n;
}

typedef struct {
    sink_t base;
    uint32_t start;     // absolute flash offset of the blob
    uint32_t limit;     // bytes available
    uint32_t pos;       // bytes accepted so far
    uint32_t fill;      // bytes in s_page
    uint32_t crc;
    bool truncated;
} flash_sink_t;

static void flash_sink_flush_page(flash_sink_t *f) {
    if (f->fill == 0) return;
    memset(s_page + f->fill, 0xFF, FLASH_PAGE_SIZE - f->fill);
    uint32_t page_off = f->start + ((f->pos - f->fill) & ~(FLASH_PAGE_SIZE - 1u));
    if ((page_off & (FLASH_SECTOR_SIZE - 1u)) == 0) flash_erase_sector(page_off);
    flash_program_page(page_off, s_page);
    f->fill = 0;
}

static void flash_write(sink_t *s, const char *buf, size_t n) {
    flash_sink_t *f = (flash_sink_t *)s;
    while (n) {
        if (f->pos >= f->limit) {
            f->truncated = true;
            return;
        }
        uint32_t room = FLASH_PAGE_SIZE - f->fill;
        uint32_t take = n < room ? (uint32_t)n : room;
        if (take > f->limit - f->pos) take = f->limit - f->pos;
        memcpy(s_page + f->fill, buf, take);
        f->crc = crc32_update(f->crc, buf, take);
        f->fill += take;
        f->pos += take;
        buf += take;
        n -= take;
        if (f->fill == FLASH_PAGE_SIZE) flash_sink_flush_page(f);
    }
}

// Files start on a page boundary so each file's first page is programmed exactly once.
static void flash_sink_align_page(flash_sink_t *f) {
    static const char pad[16] = {0};
    while (f->pos & (FLASH_PAGE_SIZE - 1u)) {
        uint32_t k = FLASH_PAGE_SIZE - (f->pos & (FLASH_PAGE_SIZE - 1u));
        flash_write(&f->base, pad, k < sizeof(pad) ? k : sizeof(pad));
        if (f->truncated) return;
    }
}

// ---- Writing a run ---------------------------------------------------------------------

static bool ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
    return a0 < b1 && b0 < a1;
}

bool store_write_run(const store_file_spec_t *files, unsigned nfiles,
                     uint32_t build_id, uint32_t nonce, uint32_t flags) {
    if (!flash_layout()->valid) {
        s_last_error = "flash layout invalid (flash size not supported or overlaps firmware)";
        return false;
    }
    if (nfiles == 0) return false;
    if (nfiles > STORE_MAX_FILES) nfiles = STORE_MAX_FILES;

    // 1. Size every file (each padded to a page boundary).
    uint32_t total = 0;
    for (unsigned i = 0; i < nfiles; i++) {
        count_sink_t c = {.base = {.write = count_write}};
        files[i].fn(&c.base);
        total += (c.count + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
    }
    const uint32_t ring = data_size();
    if (total > ring) {
        total = ring;
        flags |= STORE_RUN_TRUNCATED;
    }

    // 2. Place after the newest run, wrapping to the start of the ring if needed.
    uint32_t off = 0;
    const store_run_t *last = store_latest();
    if (last) off = align_sector(last->blob_off + last->blob_len);
    if (off + total > ring) off = 0;
    uint32_t end = align_sector(off + total);

    // 3. Retire runs that will be overwritten, and the oldest if the table is full.
    bool dropped = false;
    for (unsigned i = 0; i < s_dir.nruns;) {
        const store_run_t *r = &s_dir.runs[i];
        if (ranges_overlap(off, end, r->blob_off, align_sector(r->blob_off + r->blob_len))) {
            drop_run(i);
            dropped = true;
        } else {
            i++;
        }
    }
    if (s_dir.nruns == STORE_MAX_RUNS) {
        drop_run(0);
        dropped = true;
    }
    if (dropped && !dir_commit()) {
        s_last_error = "directory commit failed while retiring old runs";
        return false;
    }

    // 4. Stream the files into flash.
    store_run_t run;
    memset(&run, 0, sizeof(run));
    flash_sink_t fs = {
        .base = {.write = flash_write},
        .start = data_base() + off,
        .limit = total,
    };
    for (unsigned i = 0; i < nfiles; i++) {
        store_file_t *f = &run.files[run.nfiles++];
        strncpy(f->name, files[i].name, STORE_NAME_LEN - 1);
        f->offset = fs.pos;
        files[i].fn(&fs.base);
        f->size = fs.pos - f->offset;
        flash_sink_align_page(&fs);
    }
    flash_sink_flush_page(&fs);
    if (fs.truncated) flags |= STORE_RUN_TRUNCATED;

    run.run_no = s_dir.next_run_no;
    run.build_id = build_id;
    run.nonce = nonce;
    run.flags = flags;
    run.blob_off = off;
    run.blob_len = fs.pos;
    run.blob_crc = fs.crc;

    // Read back through XIP and verify before the run becomes visible.
    if (!run_crc_ok(&run)) {
        s_last_error = "read-back CRC of the written run does not match";
        return false;
    }

    // 5. Commit the directory with the new run.
    uint32_t prev_build = s_dir.last_build_id;
    s_dir.runs[s_dir.nruns++] = run;
    s_dir.next_run_no++;
    s_dir.last_build_id = build_id;
    if (!dir_commit()) {
        s_dir.nruns--;
        s_dir.next_run_no--;
        s_dir.last_build_id = prev_build;
        s_last_error = "directory commit failed";
        return false;
    }
    s_last_error = "";
    return true;
}
