/*
 * Flash inventory: identity (JEDEC, SFDP, status registers, unique ID) and the
 * configuration of the flash interface the firmware is running with
 * (RP2040 SSI, RP2350 QMI), as left by boot stage 2 / the boot ROM.
 */

#include <stdio.h>
#include <string.h>

#include "tests/tests.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "results/record.h"
#include "detect/psram_info.h"
#include "storage/flash_layout.h"
#if PICO_RP2040
#include "hardware/structs/ssi.h"
#else
#include "hardware/structs/qmi.h"
#endif

#define FIELD(v, NAME) (((v) & NAME##_BITS) >> NAME##_LSB)

// Raw SPI transaction with XIP suspended (flash_do_cmd runs from RAM).
static void flash_cmd(const uint8_t *tx, uint8_t *rx, size_t n) {
    uint32_t irq = save_and_disable_interrupts();
    flash_do_cmd(tx, rx, n);
    restore_interrupts(irq);
}

static uint8_t read_reg(uint8_t cmd) {
    uint8_t tx[2] = {cmd, 0}, rx[2] = {0};
    flash_cmd(tx, rx, 2);
    return rx[1];
}

// SFDP read: 0x5A, 24-bit address, 8 dummy clocks.
static void sfdp_read(uint32_t addr, uint8_t *out, size_t n) {
    uint8_t tx[5 + 16] = {0x5A, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 0};
    uint8_t rx[5 + 16] = {0};
    if (n > 16) n = 16;
    flash_cmd(tx, rx, 5 + n);
    memcpy(out, rx + 5, n);
}

static const char *vendor_name(uint8_t mfr, uint8_t type) {
    switch (mfr) {
    case 0xEF: return "Winbond";
    case 0x20: return type == 0x40 || type == 0x70 || type == 0x60 ? "XMC (Wuhan Xinxin) or Micron" : "Micron/XMC";
    case 0xC8: return "GigaDevice";
    case 0xC2: return "Macronix";
    case 0x9D: return "ISSI";
    case 0x68: return "Boya";
    case 0x85: return "Puya";
    case 0x0B: return "XTX";
    case 0x1C: return "EON";
    case 0x5E: return "Zbit";
    case 0xA1: return "Fudan Micro";
    case 0xBA: return "Zetta";
    case 0x01: return "Spansion/Infineon";
    case 0x00: case 0xFF: return "no response";
    default: return "unknown vendor";
    }
}

test_result_t test_flash_identify(test_ctx_t *ctx) {
    const flash_layout_t *l = flash_layout();
    test_result_t res = TR_PASS;

    rec_text(ctx, "jedec_id", "%02x %02x %02x", l->jedec[0], l->jedec[1], l->jedec[2]);
    rec_text(ctx, "vendor", "%s", vendor_name(l->jedec[0], l->jedec[1]));
    rec_value(ctx, "size_from_jedec", UNIT_BYTES, l->size_detected ? l->jedec_size : 0);
    if (l->alias_size) {
        rec_text(ctx, "address_wrap", "data at 0x%06lx reads back as offset 0: the chip is only %lu KB,"
                 " not the %lu KB it claims (counterfeit or mislabelled)",
                 (unsigned long)l->alias_size, (unsigned long)(l->alias_size / 1024),
                 (unsigned long)(l->jedec_size / 1024));
        rec_error(ctx, "jedec_size_overstated", l->alias_size, l->jedec_size, l->alias_size);
        res = TR_FAIL;
    } else {
        rec_text(ctx, "address_wrap", "none up to %lu KB (read-only probe)", (unsigned long)(l->jedec_size / 1024));
    }
    rec_value(ctx, "size_used", UNIT_BYTES, l->flash_size);
    if (l->results_area_aliased) {
        rec_text(ctx, "results_area", "ALIASES THE FIRMWARE - saving to flash is disabled");
        rec_error(ctx, "results_area_aliased", l->results_base, 0, 1);
        res = TR_FAIL;
    }
    if (l->jedec[0] == 0x00 || l->jedec[0] == 0xFF) {
        rec_error(ctx, "jedec_no_response", 0, 0, l->jedec[0]);
        res = TR_FAIL;
    }

    // SFDP: header + first parameter header (JEDEC Basic Flash Parameter Table).
    uint8_t h[16];
    sfdp_read(0, h, 16);
    uint32_t sig = h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24);
    if (sig == 0x50444653u) {   // "SFDP"
        rec_text(ctx, "sfdp", "rev %u.%u, %u parameter header(s)", h[5], h[4], h[6] + 1u);
        uint32_t ptr = h[12] | (h[13] << 8) | ((uint32_t)h[14] << 16);
        rec_text(ctx, "sfdp_bfpt", "rev %u.%u, %u dwords at 0x%06lx", h[10], h[9], h[11], (unsigned long)ptr);
        uint8_t d[8];
        sfdp_read(ptr + 4, d, 4);   // BFPT dword 2: density
        uint32_t dens = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
        uint64_t bits = (dens & 0x80000000u) ? (1ull << (dens & 0x7FFFFFFFu)) : (uint64_t)dens + 1u;
        uint32_t bytes = (uint32_t)(bits / 8u);
        rec_value(ctx, "size_from_sfdp", UNIT_BYTES, bytes);
        if (l->size_detected && bytes != l->flash_size) {
            rec_error(ctx, "sfdp_vs_jedec_size", 0, l->flash_size, bytes);
            res = TR_FAIL;
        }
    } else {
        rec_text(ctx, "sfdp", "not supported (signature 0x%08lx)", (unsigned long)sig);
    }

    uint8_t sr1 = read_reg(0x05), sr2 = read_reg(0x35), sr3 = read_reg(0x15);
    rec_text(ctx, "status_regs", "SR1 %02x SR2 %02x SR3 %02x (Winbond-style layout: QE = SR2 bit 1 -> %s)",
             sr1, sr2, sr3, (sr2 & 0x02) ? "quad enabled" : "quad not enabled");
    rec_text(ctx, "write_protect", "%s", (sr1 & 0x1C) ? "block protect bits set" : "none");

    uint8_t uid[8];
    uint32_t irq = save_and_disable_interrupts();
    flash_get_unique_id(uid);
    restore_interrupts(irq);
    rec_text(ctx, "flash_unique_id", "%02x%02x%02x%02x%02x%02x%02x%02x", uid[0], uid[1], uid[2], uid[3],
             uid[4], uid[5], uid[6], uid[7]);
    return res;
}

#if !PICO_RP2040
static const char *width_name(uint32_t w) {
    return w == 0 ? "1" : w == 1 ? "2" : w == 2 ? "4" : "?";
}
#endif

test_result_t test_flash_interface(test_ctx_t *ctx) {
    uint32_t sys_khz = clock_get_hz(clk_sys) / 1000u;
#if PICO_RP2040
    uint32_t ctrlr0 = ssi_hw->ctrlr0, baudr = ssi_hw->baudr, spi = ssi_hw->spi_ctrlr0;
    rec_text(ctx, "controller", "SSI (DW_apb_ssi), XIP via boot stage 2");
    rec_value(ctx, "ssi_ctrlr0", UNIT_HEX, ctrlr0);
    rec_value(ctx, "ssi_spi_ctrlr0", UNIT_HEX, spi);
    rec_value(ctx, "clk_divider", UNIT_COUNT, baudr);
    rec_value(ctx, "sck", UNIT_KHZ, baudr ? sys_khz / baudr : 0);
    rec_value(ctx, "rx_sample_delay", UNIT_COUNT, FIELD(ssi_hw->rx_sample_dly, SSI_RX_SAMPLE_DLY_RSD));
    static const char *const frf[] = {"standard (1-bit)", "dual", "quad", "?"};
    rec_text(ctx, "read_format", "%s, XIP cmd/mode 0x%02lx, %lu wait cycles, addr %lu bits",
             frf[FIELD(ctrlr0, SSI_CTRLR0_SPI_FRF)], (unsigned long)FIELD(spi, SSI_SPI_CTRLR0_XIP_CMD),
             (unsigned long)FIELD(spi, SSI_SPI_CTRLR0_WAIT_CYCLES), (unsigned long)FIELD(spi, SSI_SPI_CTRLR0_ADDR_L) * 4u);
    rec_text(ctx, "xip_cache", "%s", (xip_ctrl_hw->ctrl & XIP_CTRL_EN_BITS) ? "enabled (16 KB)" : "disabled");
#else
    for (int m = 0; m < 2; m++) {
        uint32_t t = qmi_hw->m[m].timing, f = qmi_hw->m[m].rfmt, c = qmi_hw->m[m].rcmd;
        char key[32];
        snprintf(key, sizeof(key), "m%d_cs%d", m, m);
        rec_text(ctx, key, "%s", m == 0 ? "flash (CS0)"
                 : g_psram.present ? "PSRAM (CS1)"
                                   : "CS1 window, unused (no PSRAM detected; registers are the boot defaults)");
        snprintf(key, sizeof(key), "m%d_timing", m);
        rec_value(ctx, key, UNIT_HEX, t);
        snprintf(key, sizeof(key), "m%d_clkdiv", m);
        uint32_t div = FIELD(t, QMI_M0_TIMING_CLKDIV);
        rec_value(ctx, key, UNIT_COUNT, div);
        snprintf(key, sizeof(key), "m%d_sck", m);
        rec_value(ctx, key, UNIT_KHZ, div ? sys_khz / div : 0);
        snprintf(key, sizeof(key), "m%d_timing_fields", m);
        rec_text(ctx, key, "rxdelay %lu, cooldown %lu, pagebreak %lu, select setup %lu hold %lu, max_select %lu, min_deselect %lu",
                 (unsigned long)FIELD(t, QMI_M0_TIMING_RXDELAY), (unsigned long)FIELD(t, QMI_M0_TIMING_COOLDOWN),
                 (unsigned long)FIELD(t, QMI_M0_TIMING_PAGEBREAK), (unsigned long)FIELD(t, QMI_M0_TIMING_SELECT_SETUP),
                 (unsigned long)FIELD(t, QMI_M0_TIMING_SELECT_HOLD), (unsigned long)FIELD(t, QMI_M0_TIMING_MAX_SELECT),
                 (unsigned long)FIELD(t, QMI_M0_TIMING_MIN_DESELECT));
        snprintf(key, sizeof(key), "m%d_xip_mode", m);
        uint32_t dw = FIELD(f, QMI_M0_RFMT_DATA_WIDTH);
        rec_text(ctx, key, "%s", dw == 2 ? "quad (1-4-4)" : dw == 1 ? "dual (1-2-2)" : "single (1-1-1)");
        snprintf(key, sizeof(key), "m%d_read_format", m);
        rec_text(ctx, key, "cmd 0x%02lx%s, widths prefix/addr/suffix/dummy/data = %s/%s/%s/%s/%s, dummy %lu bits%s%s",
                 (unsigned long)FIELD(c, QMI_M0_RCMD_PREFIX), FIELD(f, QMI_M0_RFMT_PREFIX_LEN) ? "" : " (no prefix)",
                 width_name(FIELD(f, QMI_M0_RFMT_PREFIX_WIDTH)), width_name(FIELD(f, QMI_M0_RFMT_ADDR_WIDTH)),
                 width_name(FIELD(f, QMI_M0_RFMT_SUFFIX_WIDTH)), width_name(FIELD(f, QMI_M0_RFMT_DUMMY_WIDTH)),
                 width_name(FIELD(f, QMI_M0_RFMT_DATA_WIDTH)), (unsigned long)FIELD(f, QMI_M0_RFMT_DUMMY_LEN) * 4u,
                 FIELD(f, QMI_M0_RFMT_SUFFIX_LEN) ? ", mode byte" : "", FIELD(f, QMI_M0_RFMT_DTR) ? ", DTR" : "");
    }
    char atr[128] = "";
    for (int i = 0; i < 8; i++) {
        char one[16];
        snprintf(one, sizeof(one), "%s%08lx", i ? " " : "", (unsigned long)qmi_hw->atrans[i]);
        strncat(atr, one, sizeof(atr) - strlen(atr) - 1);
    }
    rec_text(ctx, "atrans", "%s", atr);
    uint32_t xc = xip_ctrl_hw->ctrl;
    rec_text(ctx, "xip_cache", "secure %s, non-secure %s (16 KB, shared by flash and PSRAM)",
             (xc & XIP_CTRL_EN_SECURE_BITS) ? "enabled" : "disabled",
             (xc & XIP_CTRL_EN_NONSECURE_BITS) ? "enabled" : "disabled");
#endif
    return TR_PASS;
}
