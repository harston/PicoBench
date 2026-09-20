/*
 * Hardware inventory: everything that identifies the board and the conditions of the
 * run. Each function is a test stage (flag TF_INVENTORY) that only records facts;
 * HARDWARE.TXT is rendered from these records.
 */

#include <stdio.h>
#include <string.h>

#include "tests/tests.h"
#include "core/clocks_ctl.h"
#include "core/platform.h"
#include "core/timing.h"
#include "core/button.h"
#include "detect/board.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/structs/watchdog.h"
#include "hardware/vreg.h"
#include "pico/unique_id.h"
#include "pico/version.h"
#include "picobench_version.h"
#include "results/record.h"
#include "storage/flash_layout.h"
#include "storage/results_store.h"
#if PICO_RP2040
#include "hardware/structs/vreg_and_chip_reset.h"
#else
#include "hardware/structs/powman.h"
#include "pico/bootrom.h"
#endif

#define FIELD(v, NAME) (((v) & NAME##_BITS) >> NAME##_LSB)

// ---- core.chip ------------------------------------------------------------------------

test_result_t test_inv_chip(test_ctx_t *ctx) {
    uint32_t id = sysinfo_hw->chip_id;
    rec_text(ctx, "chip", "%s", platform_chip_name());
    rec_value(ctx, "chip_id", UNIT_HEX, id);
    rec_value(ctx, "part", UNIT_HEX, FIELD(id, SYSINFO_CHIP_ID_PART));
    rec_value(ctx, "revision_field", UNIT_COUNT, FIELD(id, SYSINFO_CHIP_ID_REVISION));
    rec_value(ctx, "manufacturer", UNIT_HEX, FIELD(id, SYSINFO_CHIP_ID_MANUFACTURER));
#if PICO_RP2040
    rec_text(ctx, "silicon", "RP2040-B%u", rp2040_chip_version());
    rec_text(ctx, "package", "QFN56, 30 GPIO");
    rec_value(ctx, "sram_bytes", UNIT_BYTES, 264 * 1024);
    rec_text(ctx, "sram_banks", "4 x 64 KB word-striped (SRAM0-3) + 2 x 4 KB (SRAM4-5)");
    rec_value(ctx, "dma_channels", UNIT_COUNT, 12);
    rec_value(ctx, "pio_blocks", UNIT_COUNT, 2);
#else
    uint8_t rev = rp2350_chip_version();
    rec_text(ctx, "silicon", "RP2350-A%u", rev);
    if (rev <= 2)
        rec_text(ctx, "errata_note", "RP2350-A2: errata E9 (GPIO input) and E14 (QMI/PSRAM) apply; "
                 "the SDK workarounds are enabled (PICO_RP2350_A2_SUPPORTED)");
    bool b = platform_chip() == CHIP_RP2350B;
    rec_text(ctx, "package", "%s (PACKAGE_SEL=%lu)", b ? "QFN80, 48 GPIO" : "QFN60, 30 GPIO",
             (unsigned long)sysinfo_hw->package_sel);
    rec_text(ctx, "gpio_48_confirmed", "%s", b ? "yes (RP2350B)" : "no (RP2350A)");
    rec_value(ctx, "gitref", UNIT_HEX, sysinfo_hw->gitref_rp2350);
    rec_value(ctx, "sram_bytes", UNIT_BYTES, 520 * 1024);
    rec_text(ctx, "sram_banks", "2 x 4 x 64 KB word-striped (SRAM0-3, SRAM4-7) + 2 x 4 KB (SRAM8-9)");
    rec_value(ctx, "dma_channels", UNIT_COUNT, 16);
    rec_value(ctx, "pio_blocks", UNIT_COUNT, 3);

    uint32_t info[8] = {0};
    int n = rom_get_sys_info(info, 8, SYS_INFO_CHIP_INFO | SYS_INFO_CRITICAL | SYS_INFO_CPU_INFO);
    if (n >= 6) {
        // info[0] = flags supported; then CHIP_INFO (3 words), CRITICAL, CPU_INFO.
        // Confirmed on hardware: words 2/3 are the two halves of the chip unique ID.
        rec_value(ctx, "rom_package_sel", UNIT_HEX, info[1]);
        rec_value(ctx, "rom_device_id", UNIT_HEX, info[2]);
        rec_value(ctx, "rom_wafer_id", UNIT_HEX, info[3]);
        rec_value(ctx, "otp_critical_flags", UNIT_HEX, info[4]);
        rec_text(ctx, "rom_cpu_info", "%s", info[5] ? "RISC-V" : "Arm");
    } else {
        rec_value(ctx, "rom_get_sys_info", UNIT_COUNT, n);
    }
#endif
    rec_text(ctx, "running_on", "%s", platform_arch_name());
    rec_value(ctx, "cores", UNIT_COUNT, 2);
    rec_value(ctx, "running_core", UNIT_COUNT, get_core_num());
    return TR_PASS;
}

// ---- core.firmware ---------------------------------------------------------------------

test_result_t test_inv_firmware(test_ctx_t *ctx) {
    rec_text(ctx, "version", "%s+%s%s", PICOBENCH_VERSION, PICOBENCH_GIT_SHA, PICOBENCH_GIT_DIRTY ? "-dirty" : "");
    rec_text(ctx, "build_id", "%s", PICOBENCH_BUILD_ID_STR);
    rec_text(ctx, "build_time", "%s", PICOBENCH_BUILD_TIME);
    rec_text(ctx, "platform", "%s", PICOBENCH_PLATFORM);
    rec_text(ctx, "pico_sdk", "%s", PICO_SDK_VERSION_STRING);
    rec_text(ctx, "compiler", "gcc %s", __VERSION__);
    rec_text(ctx, "board_header", "picobench (boards/picobench.h)");
#if PICO_RP2040
    rec_value(ctx, "bootrom_version", UNIT_COUNT, rp2040_rom_version());
#else
    rec_value(ctx, "bootrom_version", UNIT_COUNT, rp2350_rom_version());
    uint32_t bi[5] = {0};
    if (rom_get_sys_info(bi, 5, SYS_INFO_BOOT_INFO) >= 5) {
        rec_value(ctx, "boot_info_w0", UNIT_HEX, bi[1]);   // diagnostic partition, boot type, ...
        rec_value(ctx, "boot_diagnostic", UNIT_HEX, bi[2]);
        rec_value(ctx, "boot_reboot_param0", UNIT_HEX, bi[3]);
        rec_value(ctx, "boot_reboot_param1", UNIT_HEX, bi[4]);
    }
#endif
    return TR_PASS;
}

// ---- core.ids --------------------------------------------------------------------------

test_result_t test_inv_ids(test_ctx_t *ctx) {
    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof(id));
#if PICO_RP2040
    rec_text(ctx, "unique_id", "%s (flash chip unique ID: RP2040 has none of its own)", id);
#else
    rec_text(ctx, "unique_id", "%s (RP2350 chip ID from OTP)", id);
#endif
    return TR_PASS;
}

// ---- core.memory ------------------------------------------------------------------------

extern char __data_start__, __bss_end__, __end__, __HeapLimit;
extern char __StackLimit, __StackTop, __StackOneBottom, __StackOneTop;
extern char __flash_binary_start, __flash_binary_end;

test_result_t test_inv_memory(test_ctx_t *ctx) {
    rec_value(ctx, "fw_flash_bytes", UNIT_BYTES, &__flash_binary_end - &__flash_binary_start);
    rec_value(ctx, "fw_flash_limit", UNIT_BYTES, PICOBENCH_FW_MAX_BYTES);
    rec_text(ctx, "ram_data_bss", "0x%08lx-0x%08lx (%lu bytes)", (unsigned long)(uintptr_t)&__data_start__,
             (unsigned long)(uintptr_t)&__bss_end__, (unsigned long)(&__bss_end__ - &__data_start__));
    rec_text(ctx, "ram_heap", "0x%08lx-0x%08lx", (unsigned long)(uintptr_t)&__end__,
             (unsigned long)(uintptr_t)&__HeapLimit);
    rec_text(ctx, "core0_stack", "0x%08lx-0x%08lx", (unsigned long)(uintptr_t)&__StackLimit,
             (unsigned long)(uintptr_t)&__StackTop);
    if (&__StackOneTop == &__StackOneBottom)
        rec_text(ctx, "core1_stack", "not allocated yet (pico_multicore is linked in phase 8)");
    else
        rec_text(ctx, "core1_stack", "0x%08lx-0x%08lx", (unsigned long)(uintptr_t)&__StackOneBottom,
                 (unsigned long)(uintptr_t)&__StackOneTop);
    rec_value(ctx, "result_arena_bytes", UNIT_BYTES, record_capacity());

    const flash_layout_t *l = flash_layout();
    rec_value(ctx, "flash_scratch_offset", UNIT_HEX, l->scratch_base);
    rec_value(ctx, "results_area_offset", UNIT_HEX, l->results_base);
    rec_value(ctx, "results_area_bytes", UNIT_BYTES, l->results_size);
    rec_value(ctx, "stored_runs", UNIT_COUNT, store_run_count());
    return l->valid ? TR_PASS : TR_FAIL;
}

// ---- core.reset -------------------------------------------------------------------------

typedef struct {
    uint32_t bits;
    const char *name;
} bitname_t;

test_result_t test_inv_reset(test_ctx_t *ctx) {
#if PICO_RP2040
    static const bitname_t names[] = {
        {VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS, "power-on"},
        {VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS, "RUN pin"},
        {VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS, "debug/PSM restart"},
    };
    uint32_t r = vreg_and_chip_reset_hw->chip_reset;
#else
    static const bitname_t names[] = {
        {POWMAN_CHIP_RESET_HAD_POR_BITS, "power-on"},
        {POWMAN_CHIP_RESET_HAD_BOR_BITS, "brown-out"},
        {POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS, "RUN pin"},
        {POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS, "debug port request"},
        {POWMAN_CHIP_RESET_HAD_RESCUE_BITS, "rescue"},
        {POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS, "watchdog (powman async)"},
        {POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS, "watchdog (powman)"},
        {POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS, "watchdog (swcore)"},
        {POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS, "switched-core power-down"},
        {POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS, "glitch detector"},
        {POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS, "Hazard3 system reset request"},
        {POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_PSM_BITS, "watchdog (PSM)"},
    };
    uint32_t r = powman_hw->chip_reset;
#endif
    char buf[128] = "";
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(r & names[i].bits)) continue;
        if (buf[0]) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, names[i].name, sizeof(buf) - strlen(buf) - 1);
    }
    rec_value(ctx, "chip_reset_reg", UNIT_HEX, r);
    rec_text(ctx, "last_reset", "%s", buf[0] ? buf : "none flagged");
    rec_value(ctx, "watchdog_reason", UNIT_HEX, watchdog_hw->reason);
    rec_text(ctx, "reset_note", "chip_reset reports the last chip-level reset; a watchdog reboot "
             "(e.g. a re-run request) also shows in watchdog_reason");
    return TR_PASS;
}

// ---- core.environment ------------------------------------------------------------------

// The temperature sensor is the last ADC channel of the chip: 4 on RP2040 and on
// RP2350A, 8 on RP2350B. The build targets RP2350B, so on an RP2350A chip the compiled
// ADC_TEMPERATURE_CHANNEL_NUM (8) does not exist and reads garbage - pick it at runtime.
// (Seen on an RP2350A board: raw 55 -> "411 degC".)
static unsigned temp_channel(void) {
#if PICO_RP2040
    return ADC_TEMPERATURE_CHANNEL_NUM;
#else
    return platform_chip() == CHIP_RP2350B ? 8u : 4u;
#endif
}

// Average of 64 conversions; 12-bit, VREF assumed 3.3 V (no external reference).
static uint32_t adc_avg(unsigned input) {
    adc_select_input(input);
    (void)adc_read();
    uint32_t sum = 0;
    for (int i = 0; i < 64; i++) sum += adc_read();
    return sum / 64;
}

static uint32_t vreg_mv(unsigned vsel) {
#if PICO_RP2040
    return vsel <= 5 ? 800u : 800u + (vsel - 5u) * 50u;
#else
    return vsel <= 15 ? 550u + vsel * 50u : 0u;   // 0: above the normal range, see raw value
#endif
}

test_result_t test_inv_environment(test_ctx_t *ctx) {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    uint32_t raw = adc_avg(temp_channel());
    // Datasheet formula: T = 27 - (V - 0.706) / 0.001721, V = raw * 3.3 / 4096.
    int32_t uv = (int32_t)((uint64_t)raw * 3300000u / 4096u);
    int32_t mdeg = 27000 - (uv - 706000) * 1000 / 1721;
    rec_value(ctx, "temperature", UNIT_MILLI_C, mdeg);
    rec_value(ctx, "temp_adc_raw", UNIT_COUNT, raw);
    rec_value(ctx, "temp_adc_channel", UNIT_COUNT, temp_channel());
    if (mdeg < -40000 || mdeg > 125000) {
        rec_text(ctx, "temperature_note", "IMPLAUSIBLE - the sensor did not answer on this channel");
        rec_error(ctx, "temperature_range", temp_channel(), 27000, (uint32_t)mdeg);
    } else {
        rec_text(ctx, "temperature_note", "internal sensor, 3.3 V reference assumed: +-several degC");
    }
    adc_set_temp_sensor_enabled(false);

    unsigned vsel = (unsigned)vreg_get_voltage();
    rec_value(ctx, "vreg_vsel", UNIT_COUNT, vsel);
    rec_value(ctx, "vreg_voltage", UNIT_MV, vreg_mv(vsel));

    // VSYS/3 divider on GPIO29 (ADC3) exists on Pico / Pico 2. Not read on RP2350B,
    // where GPIO29 is a plain I/O and the ADC inputs are GPIO40-47.
    if (platform_chip() != CHIP_RP2350B) {
        adc_gpio_init(29);
        uint32_t v = adc_avg(3);
        uint32_t mv = v * 3300u * 3u / 4096u;
        rec_value(ctx, "vsys_if_pico_divider", UNIT_MV, mv);
        rec_text(ctx, "vsys_note", "%s", mv > 3000 && mv < 5600 ? "plausible VSYS: Pico-style VSYS/3 divider present"
                                                                  : "no Pico-style VSYS divider on GPIO29");
    }
    return TR_PASS;
}

// ---- clocks.sources --------------------------------------------------------------------

test_result_t test_inv_clock_sources(test_ctx_t *ctx) {
    clock_readback_t rb;
    clocks_readback(&rb);
    rec_value(ctx, "xosc_nominal", UNIT_KHZ, rb.xosc_khz);
    rec_value(ctx, "xosc_measured", UNIT_KHZ, frequency_count_khz(CLOCKS_FC0_SRC_VALUE_XOSC_CLKSRC));
    rec_value(ctx, "rosc_measured", UNIT_KHZ, frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC));
#if !PICO_RP2040
    rec_value(ctx, "lposc_measured", UNIT_KHZ, frequency_count_khz(CLOCKS_FC0_SRC_VALUE_LPOSC_CLKSRC));
#endif
    rec_text(ctx, "clk_sys", "%lu kHz from %s (ctrl 0x%08lx div 0x%08lx)", (unsigned long)rb.sys_khz,
             clocks_src_name_sys(rb.sys_ctrl), (unsigned long)rb.sys_ctrl, (unsigned long)rb.sys_div);
    rec_text(ctx, "clk_ref", "%lu kHz from %s (ctrl 0x%08lx div 0x%08lx)", (unsigned long)rb.ref_khz,
             clocks_src_name_ref(rb.ref_ctrl), (unsigned long)rb.ref_ctrl, (unsigned long)rb.ref_div);
    rec_text(ctx, "clk_peri", "%lu kHz from %s", (unsigned long)rb.peri_khz,
             clocks_src_name_aux("peri", rb.peri_ctrl));
    rec_text(ctx, "clk_usb", "%lu kHz from %s", (unsigned long)rb.usb_khz,
             clocks_src_name_aux("usb", rb.usb_ctrl));
    rec_text(ctx, "clk_adc", "%lu kHz from %s", (unsigned long)rb.adc_khz,
             clocks_src_name_aux("adc", rb.adc_ctrl));
#if PICO_RP2040
    rec_value(ctx, "clk_rtc", UNIT_KHZ, rb.aux_khz);
#else
    rec_value(ctx, "clk_hstx", UNIT_KHZ, rb.aux_khz);
#endif
    rec_text(ctx, "pll_sys", "ref/%lu x%lu /%lu/%lu = VCO %lu kHz -> %lu kHz%s", (unsigned long)rb.sys_pll.refdiv,
             (unsigned long)rb.sys_pll.fbdiv, (unsigned long)rb.sys_pll.postdiv1, (unsigned long)rb.sys_pll.postdiv2,
             (unsigned long)rb.sys_pll.vco_khz, (unsigned long)rb.sys_pll.out_khz, rb.sys_pll.locked ? "" : " NOT LOCKED");
    rec_text(ctx, "pll_usb", "ref/%lu x%lu /%lu/%lu = VCO %lu kHz -> %lu kHz%s", (unsigned long)rb.usb_pll.refdiv,
             (unsigned long)rb.usb_pll.fbdiv, (unsigned long)rb.usb_pll.postdiv1, (unsigned long)rb.usb_pll.postdiv2,
             (unsigned long)rb.usb_pll.vco_khz, (unsigned long)rb.usb_pll.out_khz, rb.usb_pll.locked ? "" : " NOT LOCKED");
    rec_text(ctx, "operating_points", "%lu %lu %lu %lu %lu MHz (default %lu)",
             (unsigned long)(clocks_op_khz(0) / 1000), (unsigned long)(clocks_op_khz(1) / 1000),
             (unsigned long)(clocks_op_khz(2) / 1000), (unsigned long)(clocks_op_khz(3) / 1000),
             (unsigned long)(clocks_op_khz(4) / 1000), (unsigned long)(clocks_op_khz(clocks_default_op()) / 1000));
    return TR_PASS;
}

// ---- usb.config ------------------------------------------------------------------------

test_result_t test_inv_usb(test_ctx_t *ctx) {
    rec_text(ctx, "device", "VID 0x1209 PID 0x0001 (pid.codes test), full speed 12 Mbit/s");
    rec_text(ctx, "functions", "CDC console + mass storage (disk PICOBENCH)");
    rec_text(ctx, "active", "only after the run (never during measurements)");
    rec_value(ctx, "clk_usb", UNIT_KHZ, frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB));
    return TR_PASS;
}

// ---- board.profile -----------------------------------------------------------------------

test_result_t test_inv_board(test_ctx_t *ctx) {
    const board_profile_t *b = board_profile();
    rec_text(ctx, "board", "%s", b->name);
    rec_text(ctx, "inferred_from", "%s", b->basis);
    rec_value(ctx, "led_gpio", UNIT_COUNT, b->led_pin);
    if (b->key_pin >= 0) rec_value(ctx, "key_gpio", UNIT_COUNT, b->key_pin);
    rec_text(ctx, "key_button", "%s", button_key_state());
    rec_text(ctx, "rerun_trigger", "hold BOOTSEL%s for 1 s while the disk is shown",
             button_key_available() ? " or KEY" : "");
    if (b->psram_cs_hint >= 0) rec_value(ctx, "psram_cs_footprint", UNIT_COUNT, b->psram_cs_hint);
    return TR_PASS;
}
