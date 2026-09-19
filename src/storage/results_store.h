/*
 * Power-loss-safe results store in flash.
 *
 * Two directory sectors (A/B) and a ring of run data. A directory is valid when its
 * magic, version, CRC32 and final COMMITTED word are all correct; the valid one with
 * the highest sequence number wins.
 *
 * Writing a run:
 *   1. size every file with a dry run of the writers (counting sink),
 *   2. pick a free range after the newest run (wrap to the start of the ring if needed),
 *   3. if older runs overlap that range, commit a directory without them first,
 *   4. erase + program the files, read back and verify the CRC32,
 *   5. write the new directory to the INACTIVE slot, programming COMMITTED last.
 * Power loss anywhere leaves the previous directory, and every run it lists, intact.
 */

#ifndef PICOBENCH_RESULTS_STORE_H
#define PICOBENCH_RESULTS_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "results/sink.h"

#define STORE_MAX_RUNS  16
#define STORE_MAX_FILES 8
#define STORE_NAME_LEN  16

#define STORE_RUN_PASS       0x1u   // overall verdict PASS
#define STORE_RUN_TRUNCATED  0x2u   // did not fit, some file content is missing
#define STORE_RUN_CRASHES    0x4u   // run contains TEST_CRASHED records

typedef struct {
    char name[STORE_NAME_LEN];   // "REPORT.TXT" (8.3, upper case)
    uint32_t offset;             // from the start of the run blob
    uint32_t size;
} store_file_t;

typedef struct {
    uint32_t run_no;             // 1, 2, 3, ... never reused
    uint32_t build_id;
    uint32_t nonce;
    uint32_t flags;              // STORE_RUN_*
    uint32_t blob_off;           // offset within the data ring
    uint32_t blob_len;
    uint32_t blob_crc;
    uint32_t nfiles;
    store_file_t files[STORE_MAX_FILES];
} store_run_t;

// A file generator: writes the whole file to `out`. It is called twice (size, then
// write) and must produce identical output both times.
typedef void (*store_writer_fn)(sink_t *out);

typedef struct {
    const char *name;
    store_writer_fn fn;
} store_file_spec_t;

// Read the directory. Runs whose data fail the CRC check are dropped from view.
void store_mount(void);

unsigned store_run_count(void);
const store_run_t *store_run(unsigned i);      // 0 = oldest
const store_run_t *store_latest(void);         // NULL if none
uint32_t store_next_run_no(void);
uint32_t store_dir_seq(void);
uint32_t store_invalid_runs(void);             // runs hidden because of bad CRC

// Build ID that last committed a run. Survives deleting runs, so deleting results from
// the disk does not make the next boot re-run the tests.
uint32_t store_last_build_id(void);

// Remove runs by number and commit the directory (A/B, power-loss safe). The run data
// stay in flash until their space is reused.
bool store_delete_runs(const uint32_t *run_nos, unsigned n);

// Why the last store_write_run()/store_delete_runs() failed ("" after success).
const char *store_last_error(void);

// XIP address of a file's content.
const uint8_t *store_file_data(const store_run_t *run, const store_file_t *f);

// Generate and commit a new run. Returns false if nothing could be committed.
bool store_write_run(const store_file_spec_t *files, unsigned nfiles,
                     uint32_t build_id, uint32_t nonce, uint32_t flags);

#endif
