// Result of PSRAM detection (psram.detect), used by the PSRAM tests (phase 6).
// After a crash-resume the detection stage is not repeated: psram_resume() restores it
// from a copy kept in uninitialised RAM (tied to the run nonce). Stages must still
// re-check `present` and treat PSRAM as absent if it is not set.
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

/*
 * Re-apply the SDK PSRAM profile for the current clk_sys and re-initialise the QMI CS1
 * window. Needed after anything that reconfigures QMI, and after every clk_sys change
 * (the PSRAM clock is clk_sys / divisor).
 *
 * In particular the boot ROM's rom_flash_select_xip_read_mode() configures BOTH chip
 * selects, so the XIP mode sweep leaves the PSRAM window set up as if it were flash.
 * Returns false if PSRAM is absent or the re-init failed.
 */
bool psram_reapply(void);

/*
 * Retime the PSRAM window for clk_sys = clk_hz by writing QMI M1 TIMING directly, by the
 * SDK's own rules (psram_configure_params) with SCK at most max_sck_hz. Unlike
 * psram_reapply() it does not go through flash_start_xip(), which also puts the flash
 * window back to the boot ROM's setting - wrong once clk_sys is far above the default
 * (overclocking variant). No PSRAM access may be in flight. Returns the divisor, or 0
 * if PSRAM is absent or the timing does not fit the register.
 */
uint32_t psram_retime_direct(uint32_t clk_hz, uint32_t max_sck_hz);

/*
 * After a watchdog reset in the middle of a run: bring back what psram.detect found in
 * this run (CS pin function, FLASH_DEVINFO CS1, SDK profile) and check that the chip
 * still answers READ ID. Without it every later PSRAM stage reported N/A and the CS pin
 * was left in its reset state (seen in the overclocking runs, results_10). Returns true
 * if PSRAM is present and working again.
 */
bool psram_resume(uint32_t run_nonce);

#endif
