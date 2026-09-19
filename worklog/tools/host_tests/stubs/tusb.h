// Host stub: just enough of TinyUSB for msc_disk.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define SCSI_SENSE_DATA_PROTECT 0x07
#define SCSI_SENSE_ILLEGAL_REQUEST 0x05
#define SCSI_SENSE_MEDIUM_ERROR 0x03
#define SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1E
static inline bool tud_msc_set_sense(uint8_t lun, uint8_t k, uint8_t a, uint8_t q) { (void)lun; (void)k; (void)a; (void)q; return true; }
