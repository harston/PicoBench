// Result of PSRAM detection (psram.detect), used by the PSRAM tests (phase 6).
// RAM only: after a crash-resume the detection stage is not repeated, so later
// stages must re-check `present` and treat PSRAM as absent if it is not set.
#ifndef PICOBENCH_PSRAM_INFO_H
#define PICOBENCH_PSRAM_INFO_H

#include <stdbool.h>
#include <stdint.h>

// XIP window of QMI CS1: cached and uncached (no cache allocation) aliases.
#define PSRAM_CACHED_BASE  0x11000000u
#define PSRAM_NOCACHE_BASE 0x15000000u

typedef struct {
    bool present;
    bool initialised;
    uint8_t cs_gpio;
    uint8_t id[3];      // MFID, KGD, EID
    uint32_t size;      // bytes
} psram_info_t;

extern psram_info_t g_psram;

#endif
