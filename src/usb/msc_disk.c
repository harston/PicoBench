/*
 * Virtual FAT16 volume "PICOBENCH".
 *
 * Nothing is stored as a FAT image. At mount time a small object table is built from
 * the results-store directory; every sector the host reads is generated on the fly:
 *   boot sector, both FAT copies, directories -> computed from the object table
 *   file data                                 -> straight from flash (XIP) or RAM
 *
 * Writes: sectors the host writes go to a RAM overlay (reads return the overlay copy),
 * so the host sees a normal, consistent, writable disk during the session. Only one
 * kind of change is made persistent: when the directory entry of RUNS\RUNxxxx (or of
 * RUNS itself) disappears, those runs are deleted from the results store, using the
 * power-loss-safe directory commit. Everything else in the overlay is lost on reset.
 *
 * Tree:
 *   README.TXT, STATUS.TXT     (firmware constant / generated at boot)
 *   REPORT.TXT, RESULTS.CSV,   (files of the latest run)
 *   ERRORS.TXT, ...
 *   RUNS\RUN0001\...           (every stored run)
 *   .Trashes (file), .metadata_never_index, .fseventsd\no_log   (hidden, for macOS:
 *     Finder deletes immediately, no Spotlight index, no fsevents logs - the same trick
 *     CircuitPython uses; they keep the RAM overlay small)
 *
 * Geometry: 16 MB, 512-byte sectors, 1 sector per cluster, 2 FATs x 128 sectors,
 * 512 root entries. 32479 clusters -> FAT16 by cluster count.
 */

#include "usb/msc_disk.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "tusb.h"
#include "picobench_version.h"
#include "storage/results_store.h"

#define SECTOR_SIZE      512u
#define TOTAL_SECTORS    32768u               // 16 MB
#define RESERVED_SECTORS 1u
#define NUM_FATS         2u
#define FAT_SECTORS      128u
#define ROOT_ENTRIES     512u
#define ROOT_SECTORS     (ROOT_ENTRIES * 32u / SECTOR_SIZE)
#define FAT1_START       RESERVED_SECTORS
#define ROOT_START       (RESERVED_SECTORS + NUM_FATS * FAT_SECTORS)
#define DATA_START       (ROOT_START + ROOT_SECTORS)
#define CLUSTER_COUNT    (TOTAL_SECTORS - DATA_START)
#define FAT_EOC          0xFFFFu

#define ATTR_HIDDEN      0x02
#define ATTR_VOLUME_ID   0x08
#define ATTR_DIRECTORY   0x10
#define ATTR_ARCHIVE     0x20
#define ATTR_LFN         0x0F

#define MAX_OBJECTS      (8 + STORE_MAX_FILES + 1 + STORE_MAX_RUNS * (1 + STORE_MAX_FILES))
#define ROOT_PARENT      0xFFFFu
#define MAX_OVERLAY      256u                 // sectors, bounded further by the buffer

// Deletions are applied once the host has been quiet for this long.
#define SCAN_DELAY_US    300000

typedef enum { OBJ_DIR, OBJ_FILE } obj_kind_t;

typedef struct {
    char name83[11];           // space-padded 8.3 name
    uint8_t kind;              // obj_kind_t
    uint8_t attr;              // extra attributes (hidden)
    uint8_t lower;             // NT case flags (byte 12): 0x08 base / 0x10 ext in lower case
    uint16_t parent;           // object index of the parent directory, ROOT_PARENT = root
    const char *lfn;           // long file name or NULL
    uint32_t first_cluster;    // 0 for empty files
    uint32_t nclusters;
    uint32_t size;             // files only
    const uint8_t *data;       // files only (XIP or RAM)
    uint32_t run_no;           // run directories: run number, else 0
    bool deleted;              // run directory already deleted from the store
} vobj_t;

static vobj_t s_obj[MAX_OBJECTS];
static unsigned s_nobj;
static unsigned s_runs_dir = MAX_OBJECTS;
static uint16_t s_fat_date, s_fat_time;
static uint32_t s_serial;
static uint32_t s_used_end;     // first cluster after all allocated objects

static uint8_t *s_ovl_data;
static uint32_t s_ovl_cap;
static uint32_t s_ovl_lba[MAX_OVERLAY];
static uint32_t s_ovl_n;
static bool s_scan_needed;
static absolute_time_t s_last_write;
static uint32_t s_deleted_runs;

static const char s_readme[] =
    "PicoBench - Pico Hardware Characterization & Benchmark\r\n"
    "=====================================================\r\n"
    "\r\n"
    "This disk shows the benchmark reports stored in the board's flash.\r\n"
    "\r\n"
    "  REPORT.TXT    human-readable report of the latest run\r\n"
    "  RESULTS.CSV   every measurement, one row per record (Excel, Python)\r\n"
    "  ERRORS.TXT    crashes, failed tests and memory errors of the latest run\r\n"
    "  STATUS.TXT    what the current boot did (tests run? report saved?)\r\n"
    "  RUNS\\         all stored runs (RUN0001, RUN0002, ...), up to 16, oldest dropped\r\n"
    "\r\n"
    "Deleting a RUNS\\RUNxxxx folder (or RUNS) deletes those runs from flash.\r\n"
    "Other changes to this disk are kept in RAM only and vanish after a restart.\r\n"
    "\r\n"
    "LED:\r\n"
    "  fast blink (~4 Hz)          tests running - do not unplug\r\n"
    "  solid on                    writing the report to flash - do not unplug\r\n"
    "  1 short blink every 2 s     finished, PASS - safe to unplug\r\n"
    "  2 short blinks every 2 s    finished with errors, report saved - safe to unplug\r\n"
    "\r\n"
    "The tests run once after a new firmware is flashed. To run them again, hold the\r\n"
    "BOOTSEL button (or KEY on the WeAct RP2350B) for about 1 second while this disk is\r\n"
    "shown, or send 'r' on the PicoBench serial console.\r\n"
    "\r\n"
    "Firmware " PICOBENCH_VERSION "+" PICOBENCH_GIT_SHA ", build " PICOBENCH_BUILD_ID_STR
    ", " PICOBENCH_PLATFORM "\r\n";

// ---- Object table ------------------------------------------------------------------------

static void to_83(char out[11], const char *name) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    for (size_t i = 0; i < base && i < 8; i++) out[i] = name[i];
    if (dot) {
        for (size_t i = 0; i < 3 && dot[1 + i]; i++) out[8 + i] = dot[1 + i];
    }
    for (int i = 0; i < 11; i++) {
        if (out[i] >= 'a' && out[i] <= 'z') out[i] = (char)(out[i] - 'a' + 'A');
    }
}

static unsigned add_obj(const char *name, obj_kind_t kind, uint16_t parent,
                        const uint8_t *data, uint32_t size) {
    if (s_nobj >= MAX_OBJECTS) return MAX_OBJECTS;
    vobj_t *o = &s_obj[s_nobj];
    memset(o, 0, sizeof(*o));
    to_83(o->name83, name);
    o->kind = (uint8_t)kind;
    o->parent = parent;
    o->data = data;
    o->size = size;
    return s_nobj++;
}

// Hidden object with a long name that has no valid 8.3 form (leading dot).
static unsigned add_hidden(const char *lfn, const char short83[12], obj_kind_t kind, uint16_t parent) {
    unsigned i = add_obj("", kind, parent, NULL, 0);
    if (i >= MAX_OBJECTS) return i;
    memcpy(s_obj[i].name83, short83, 11);
    s_obj[i].lfn = lfn;
    s_obj[i].attr = ATTR_HIDDEN;
    return i;
}

static unsigned lfn_entries(const vobj_t *o) {
    return o->lfn ? (unsigned)((strlen(o->lfn) + 12u) / 13u) : 0u;
}

// Number of 32-byte entries of an object in its parent directory.
static unsigned obj_entries(const vobj_t *o) {
    return 1u + lfn_entries(o);
}

static unsigned dir_entry_count(unsigned dir) {
    unsigned n = 2;   // "." and ".."
    for (unsigned i = 0; i < s_nobj; i++) {
        if (s_obj[i].parent == dir) n += obj_entries(&s_obj[i]);
    }
    return n;
}

static void add_run_files(const store_run_t *run, uint16_t parent) {
    for (uint32_t f = 0; f < run->nfiles; f++) {
        const store_file_t *sf = &run->files[f];
        add_obj(sf->name, OBJ_FILE, parent, store_file_data(run, sf), sf->size);
    }
}

// "2026-09-19T12:53:20Z" -> FAT date/time. All entries carry the firmware build time:
// the board has no real-time clock.
// (No sscanf: it pulls in newlib FILE code that does not link with pqt-gcc.)
static unsigned num_at(const char *s, unsigned pos, unsigned digits) {
    unsigned v = 0;
    for (unsigned i = 0; i < digits; i++) v = v * 10u + (unsigned)(s[pos + i] - '0');
    return v;
}

static void parse_build_time(void) {
    const char *t = PICOBENCH_BUILD_TIME;   // fixed format YYYY-MM-DDTHH:MM:SSZ
    unsigned y = num_at(t, 0, 4), mo = num_at(t, 5, 2), d = num_at(t, 8, 2);
    unsigned h = num_at(t, 11, 2), mi = num_at(t, 14, 2), s = num_at(t, 17, 2);
    s_fat_date = (uint16_t)(((y - 1980u) << 9) | (mo << 5) | d);
    s_fat_time = (uint16_t)((h << 11) | (mi << 5) | (s / 2u));
}

void msc_disk_build(void *overlay, uint32_t overlay_bytes, const char *status, uint32_t status_len) {
    s_nobj = 0;
    s_ovl_n = 0;
    s_scan_needed = false;
    s_deleted_runs = 0;
    s_ovl_data = overlay;
    s_ovl_cap = overlay_bytes / SECTOR_SIZE;
    if (s_ovl_cap > MAX_OVERLAY) s_ovl_cap = MAX_OVERLAY;
    parse_build_time();
    s_serial = PICOBENCH_BUILD_ID ^ store_dir_seq();

    add_obj("README.TXT", OBJ_FILE, ROOT_PARENT, (const uint8_t *)s_readme, sizeof(s_readme) - 1);
    if (status) add_obj("STATUS.TXT", OBJ_FILE, ROOT_PARENT, (const uint8_t *)status, status_len);
    const store_run_t *latest = store_latest();
    if (latest) add_run_files(latest, ROOT_PARENT);

    s_runs_dir = add_obj("RUNS", OBJ_DIR, ROOT_PARENT, NULL, 0);
    for (unsigned i = 0; i < store_run_count(); i++) {
        const store_run_t *run = store_run(i);
        char name[12];
        snprintf(name, sizeof(name), "RUN%04lu", (unsigned long)(run->run_no % 10000u));
        unsigned d = add_obj(name, OBJ_DIR, (uint16_t)s_runs_dir, NULL, 0);
        if (d < MAX_OBJECTS) s_obj[d].run_no = run->run_no;
        add_run_files(run, (uint16_t)d);
    }

    add_hidden(".metadata_never_index", "METADA~1   ", OBJ_FILE, ROOT_PARENT);
    add_hidden(".Trashes", "TRASHE~1   ", OBJ_FILE, ROOT_PARENT);
    unsigned fsev = add_hidden(".fseventsd", "FSEVEN~1   ", OBJ_DIR, ROOT_PARENT);
    unsigned nolog = add_obj("NO_LOG", OBJ_FILE, (uint16_t)fsev, NULL, 0);
    if (nolog < MAX_OBJECTS) s_obj[nolog].lower = 0x08;   // shown as "no_log"

    // Allocate clusters: directories first, then files, in object order.
    uint32_t next = 2;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < s_nobj; i++) {
            vobj_t *o = &s_obj[i];
            if ((pass == 0) != (o->kind == OBJ_DIR)) continue;
            uint32_t bytes = o->kind == OBJ_DIR ? dir_entry_count(i) * 32u : o->size;
            o->nclusters = (bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE;
            if (next + o->nclusters > CLUSTER_COUNT + 2u) o->nclusters = 0;   // cannot happen
            o->first_cluster = o->nclusters ? next : 0;
            next += o->nclusters;
        }
    }
    s_used_end = next;
}

// Object owning cluster `c`, or NULL for a free cluster.
static const vobj_t *obj_for_cluster(uint32_t c) {
    if (c >= s_used_end) return NULL;   // the vast majority of the 16 MB volume
    for (unsigned i = 0; i < s_nobj; i++) {
        const vobj_t *o = &s_obj[i];
        if (o->nclusters && c >= o->first_cluster && c < o->first_cluster + o->nclusters) return o;
    }
    return NULL;
}

// ---- Sector generators -----------------------------------------------------------------

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void boot_sector(uint8_t *b) {
    static const uint8_t jump[3] = {0xEB, 0x3C, 0x90};
    memcpy(b, jump, 3);
    memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 11, SECTOR_SIZE);
    b[13] = 1;                                  // sectors per cluster
    put16(b + 14, RESERVED_SECTORS);
    b[16] = NUM_FATS;
    put16(b + 17, ROOT_ENTRIES);
    put16(b + 19, TOTAL_SECTORS);
    b[21] = 0xF8;                               // fixed disk
    put16(b + 22, FAT_SECTORS);
    put16(b + 24, 63);                          // sectors per track (unused)
    put16(b + 26, 255);                         // heads (unused)
    b[36] = 0x80;                               // drive number
    b[38] = 0x29;                               // extended boot signature
    put32(b + 39, s_serial);
    memcpy(b + 43, "PICOBENCH  ", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55;
    b[511] = 0xAA;
}

static void fat_sector(uint32_t fat_sector_idx, uint8_t *b) {
    uint32_t first = fat_sector_idx * (SECTOR_SIZE / 2u);
    for (uint32_t k = 0; k < SECTOR_SIZE / 2u; k++) {
        uint32_t c = first + k;
        uint16_t v = 0;
        if (c == 0) {
            v = 0xFFF8;                           // media descriptor
        } else if (c == 1) {
            v = FAT_EOC;
        } else {
            const vobj_t *o = obj_for_cluster(c);
            if (o) v = (c + 1u < o->first_cluster + o->nclusters) ? (uint16_t)(c + 1u) : FAT_EOC;
        }
        put16(b + 2u * k, v);
    }
}

static void dir_entry(uint8_t *e, const char name83[11], uint8_t attr, uint8_t lower,
                      uint32_t cluster, uint32_t size) {
    memcpy(e, name83, 11);
    e[11] = attr;
    e[12] = lower;
    put16(e + 14, s_fat_time);                  // creation
    put16(e + 16, s_fat_date);
    put16(e + 18, s_fat_date);                  // last access
    put16(e + 22, s_fat_time);                  // modification
    put16(e + 24, s_fat_date);
    put16(e + 26, (uint16_t)cluster);
    put32(e + 28, size);
}

static uint8_t sfn_checksum(const char name83[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1u) << 7) + (sum >> 1) + (uint8_t)name83[i]);
    return sum;
}

// VFAT long-name entry `seq` (1-based) of `lfn`, 13 UCS-2 characters per entry.
static void lfn_entry(uint8_t *e, const char *lfn, unsigned seq, bool last, uint8_t csum) {
    static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    size_t len = strlen(lfn);
    e[0] = (uint8_t)(seq | (last ? 0x40u : 0u));
    e[11] = ATTR_LFN;
    e[12] = 0;
    e[13] = csum;
    for (unsigned i = 0; i < 13; i++) {
        size_t idx = (seq - 1u) * 13u + i;
        uint16_t ch = idx < len ? (uint8_t)lfn[idx] : (idx == len ? 0x0000u : 0xFFFFu);
        put16(e + pos[i], ch);
    }
}

// Directory entry number `idx` of directory `dir` (ROOT_PARENT for root). False past end.
static bool dir_entry_at(unsigned dir, unsigned idx, uint8_t *e) {
    if (dir == ROOT_PARENT) {
        if (idx == 0) {
            dir_entry(e, "PICOBENCH  ", ATTR_VOLUME_ID, 0, 0, 0);
            return true;
        }
        idx -= 1;
    } else {
        const vobj_t *self = &s_obj[dir];
        if (idx == 0) {
            dir_entry(e, ".          ", ATTR_DIRECTORY, 0, self->first_cluster, 0);
            return true;
        }
        if (idx == 1) {
            uint32_t pc = self->parent == ROOT_PARENT ? 0 : s_obj[self->parent].first_cluster;
            dir_entry(e, "..         ", ATTR_DIRECTORY, 0, pc, 0);
            return true;
        }
        idx -= 2;
    }
    for (unsigned i = 0; i < s_nobj; i++) {
        const vobj_t *o = &s_obj[i];
        if (o->parent != dir) continue;
        unsigned n = obj_entries(o);
        if (idx >= n) {
            idx -= n;
            continue;
        }
        unsigned nl = n - 1u;
        if (idx < nl) {   // long-name entries come first, highest sequence number first
            lfn_entry(e, o->lfn, nl - idx, idx == 0, sfn_checksum(o->name83));
        } else if (o->kind == OBJ_DIR) {
            dir_entry(e, o->name83, ATTR_DIRECTORY | o->attr, o->lower, o->first_cluster, 0);
        } else {
            dir_entry(e, o->name83, ATTR_ARCHIVE | o->attr, o->lower, o->first_cluster, o->size);
        }
        return true;
    }
    return false;
}

static void dir_sector(unsigned dir, uint32_t sector_in_dir, uint8_t *b) {
    for (unsigned k = 0; k < SECTOR_SIZE / 32u; k++) {
        if (!dir_entry_at(dir, sector_in_dir * (SECTOR_SIZE / 32u) + k, b + 32u * k)) break;
    }
}

static void generate_sector(uint32_t lba, uint8_t *b) {
    memset(b, 0, SECTOR_SIZE);
    if (lba == 0) {
        boot_sector(b);
    } else if (lba < ROOT_START) {
        fat_sector((lba - FAT1_START) % FAT_SECTORS, b);
    } else if (lba < DATA_START) {
        dir_sector(ROOT_PARENT, lba - ROOT_START, b);
    } else if (lba < TOTAL_SECTORS) {
        uint32_t c = lba - DATA_START + 2u;
        const vobj_t *o = obj_for_cluster(c);
        if (!o) return;
        uint32_t rel = c - o->first_cluster;
        if (o->kind == OBJ_DIR) {
            dir_sector((unsigned)(o - s_obj), rel, b);
        } else {
            uint32_t off = rel * SECTOR_SIZE;
            uint32_t n = o->size - off < SECTOR_SIZE ? o->size - off : SECTOR_SIZE;
            memcpy(b, o->data + off, n);
        }
    }
}

// ---- RAM overlay ---------------------------------------------------------------------------

static int overlay_find(uint32_t lba) {
    for (uint32_t i = 0; i < s_ovl_n; i++) {
        if (s_ovl_lba[i] == lba) return (int)i;
    }
    return -1;
}

static void read_sector(uint32_t lba, uint8_t *b) {
    int i = overlay_find(lba);
    if (i >= 0) memcpy(b, s_ovl_data + (uint32_t)i * SECTOR_SIZE, SECTOR_SIZE);
    else generate_sector(lba, b);
}

// ---- Deletion detection ----------------------------------------------------------------------

// Is there a live directory entry pointing at `cluster` in the directory stored in
// sectors [lba, lba + nsectors)?
static bool dir_has_live_entry(uint32_t lba, uint32_t nsectors, uint32_t cluster) {
    uint8_t b[SECTOR_SIZE];
    for (uint32_t s = 0; s < nsectors; s++) {
        read_sector(lba + s, b);
        for (unsigned k = 0; k < SECTOR_SIZE / 32u; k++) {
            const uint8_t *e = b + 32u * k;
            if (e[0] == 0x00) return false;           // end of directory
            if (e[0] == 0xE5 || e[11] == ATTR_LFN) continue;
            if ((e[11] & ATTR_DIRECTORY) && get16(e + 26) == cluster) return true;
        }
    }
    return false;
}

static void scan_deletions(void) {
    if (s_runs_dir >= s_nobj) return;
    const vobj_t *runs = &s_obj[s_runs_dir];
    bool runs_alive = runs->nclusters &&
                      dir_has_live_entry(ROOT_START, ROOT_SECTORS, runs->first_cluster);
    uint32_t doomed[STORE_MAX_RUNS];
    unsigned n = 0;
    for (unsigned i = 0; i < s_nobj && n < STORE_MAX_RUNS; i++) {
        vobj_t *o = &s_obj[i];
        if (o->kind != OBJ_DIR || o->parent != s_runs_dir || o->deleted || !o->run_no) continue;
        bool alive = runs_alive &&
                     dir_has_live_entry(DATA_START + runs->first_cluster - 2u, runs->nclusters,
                                        o->first_cluster);
        if (!alive) doomed[n++] = o->run_no;
    }
    if (n && store_delete_runs(doomed, n)) {
        for (unsigned i = 0; i < s_nobj; i++) {
            for (unsigned k = 0; k < n; k++) {
                if (s_obj[i].run_no == doomed[k]) s_obj[i].deleted = true;
            }
        }
        s_deleted_runs += n;
    }
}

bool msc_disk_task(void) {
    if (!s_scan_needed || absolute_time_diff_us(s_last_write, get_absolute_time()) < SCAN_DELAY_US)
        return false;
    s_scan_needed = false;
    uint32_t before = s_deleted_runs;
    scan_deletions();
    return s_deleted_runs != before;
}

uint32_t msc_disk_deleted_runs(void) {
    return s_deleted_runs;
}

// ---- TinyUSB MSC callbacks -------------------------------------------------------------

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "PicoBnch", 8);
    memcpy(product_id, "Report Disk     ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = TOTAL_SECTORS;
    *block_size = SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    static uint8_t sector[SECTOR_SIZE];
    uint8_t *out = buffer;
    uint32_t done = 0;
    while (done < bufsize) {
        uint32_t abs = offset + done;
        uint32_t l = lba + abs / SECTOR_SIZE;
        uint32_t in = abs % SECTOR_SIZE;
        read_sector(l, sector);
        uint32_t n = SECTOR_SIZE - in;
        if (n > bufsize - done) n = bufsize - done;
        memcpy(out + done, sector + in, n);
        done += n;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    uint32_t done = 0;
    while (done < bufsize) {
        uint32_t abs = offset + done;
        uint32_t l = lba + abs / SECTOR_SIZE;
        uint32_t in = abs % SECTOR_SIZE;
        uint32_t n = SECTOR_SIZE - in;
        if (n > bufsize - done) n = bufsize - done;
        if (l >= TOTAL_SECTORS) {
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);   // LBA out of range
            return -1;
        }
        int i = overlay_find(l);
        if (i < 0) {
            if (s_ovl_n >= s_ovl_cap) {
                tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);  // write error
                return -1;
            }
            i = (int)s_ovl_n;
            uint8_t *slot = s_ovl_data + (uint32_t)i * SECTOR_SIZE;
            generate_sector(l, slot);   // partial writes keep the rest of the sector
            s_ovl_lba[s_ovl_n++] = l;
        }
        memcpy(s_ovl_data + (uint32_t)i * SECTOR_SIZE + in, buffer + done, n);
        done += n;
        if (l >= ROOT_START) s_scan_needed = true;   // directories may have changed
    }
    s_last_write = get_absolute_time();
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}
