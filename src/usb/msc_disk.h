#ifndef PICOBENCH_MSC_DISK_H
#define PICOBENCH_MSC_DISK_H

#include <stdbool.h>
#include <stdint.h>

// Build the virtual FAT volume from the mounted results store. Call before tud_init().
//   overlay        RAM for sectors the host writes (reused measurement buffers)
//   status         STATUS.TXT content (RAM, must stay valid), or NULL
void msc_disk_build(void *overlay, uint32_t overlay_bytes, const char *status, uint32_t status_len);

// Call from the main loop: applies deletions of RUNS\RUNxxxx to the results store once
// the host has been quiet for a moment. Returns true when runs were deleted just now.
bool msc_disk_task(void);

uint32_t msc_disk_deleted_runs(void);

#endif
