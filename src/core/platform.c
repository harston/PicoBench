#include "core/platform.h"

#include "core/sequencer.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/regs/sysinfo.h"

uint32_t platform_chip_id_raw(void) {
    return sysinfo_hw->chip_id;
}

unsigned platform_chip_revision(void) {
    return (sysinfo_hw->chip_id & SYSINFO_CHIP_ID_REVISION_BITS) >> SYSINFO_CHIP_ID_REVISION_LSB;
}

uint8_t platform_chip(void) {
#if PICO_RP2040
    return CHIP_RP2040;
#else
    // PACKAGE_SEL: 0 = QFN80 (RP2350B, 48 GPIO), 1 = QFN60 (RP2350A, 30 GPIO).
    return (sysinfo_hw->package_sel & SYSINFO_PACKAGE_SEL_BITS) ? CHIP_RP2350A : CHIP_RP2350B;
#endif
}

const char *platform_chip_name(void) {
    switch (platform_chip()) {
    case CHIP_RP2040: return "RP2040";
    case CHIP_RP2350A: return "RP2350A";
    case CHIP_RP2350B: return "RP2350B";
    default: return "?";
    }
}

const char *platform_arch_name(void) {
#if PICO_RP2040
    return "Cortex-M0+";
#elif defined(__riscv)
    return "Hazard3 (RV32IMAC+)";
#else
    return "Cortex-M33";
#endif
}
