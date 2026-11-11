// Built when there is no local ARM workload (workload/arm_workload.c).
//
// The workload is excerpts of commercial Harmony cartridge games, so it is never part
// of the repository: worklog/tools/arm_workload/awl_tool.py records it from ROM files
// the user has (see workload/README.md). Without it cpu.thumb_interp reports N/A and
// hotpath.arm_neighbour runs its built-in synthetic load only (synth_load.h).
#include <stdint.h>

const uint32_t g_arm_workload_words = 0u;
const uint32_t g_arm_workload[1] = {0u};
