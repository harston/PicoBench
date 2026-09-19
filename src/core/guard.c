#include "core/guard.h"

#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "picobench_version.h"

// Mixed with the build ID: scratch state left behind by a different firmware build
// (e.g. flashed via a soft reboot into BOOTSEL mid-run) is ignored, not mistaken for
// a crash of this build.
#define GUARD_MAGIC (0x50424731u ^ PICOBENCH_BUILD_ID)   // "PBG1"
#define RERUN_MAGIC (0x50425252u ^ PICOBENCH_BUILD_ID)   // "PBRR"

void guard_boot(guard_boot_info_t *out) {
    out->watchdog_reason = watchdog_hw->reason;
    out->watchdog_reboot = watchdog_caused_reboot();
    out->crashed = false;
    out->crashed_stage = 0;
    out->run_nonce = 0;
    out->crash_count = 0;
    out->rerun_requested = false;

    if (watchdog_hw->scratch[0] == RERUN_MAGIC) {
        watchdog_hw->scratch[0] = 0;
        out->rerun_requested = true;
        return;
    }
    if (watchdog_hw->scratch[0] != GUARD_MAGIC) return;

    uint32_t stage_plus_1 = watchdog_hw->scratch[1];
    out->run_nonce = watchdog_hw->scratch[2];
    out->crash_count = watchdog_hw->scratch[3];
    if (stage_plus_1 != 0) {
        // Any reset in the middle of a stage counts: watchdog timeout, hard fault
        // ending in a reset, or a manual reset. Power loss clears scratch instead.
        out->crashed = true;
        out->crashed_stage = stage_plus_1 - 1;
        out->crash_count++;
    }
}

void guard_run_start(uint32_t run_nonce, uint32_t crash_count) {
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[2] = run_nonce;
    watchdog_hw->scratch[3] = crash_count;
    watchdog_hw->scratch[0] = GUARD_MAGIC;
    watchdog_enable(GUARD_TIMEOUT_MS, true);
}

void guard_stage_begin(uint32_t stage) {
    watchdog_hw->scratch[1] = stage + 1;
    watchdog_update();
}

void guard_stage_end(void) {
    watchdog_hw->scratch[1] = 0;
    watchdog_update();
}

void guard_run_end(void) {
    watchdog_disable();
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[2] = 0;
    watchdog_hw->scratch[3] = 0;
}

void guard_request_rerun(void) {
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[0] = RERUN_MAGIC;
    watchdog_reboot(0, 0, 10);
    for (;;) {
        __wfi();
    }
}
