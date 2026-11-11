#include "core/clocks_ctl.h"

#include <string.h>

#include "core/led.h"
#include "core/timing.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/regs/pll.h"
#include "hardware/vreg.h"
#include "hardware/xip_cache.h"
#include "pico/time.h"
#if PICO_RP2040
#include "hardware/structs/ssi.h"
#else
#include "hardware/structs/qmi.h"
#endif

#if PICO_RP2040
static const uint32_t s_ops_khz[] = {48000, 96000, 120000, 125000, 133000, 150000, 200000};
#define DEFAULT_OP 3
// Boot stage 2 runs the flash at clk_sys / 2: proven up to 133 MHz (SCK 66.5 MHz).
#define BOOT_FLASH_MAX_KHZ 133000u
// Above this the RP2040 needs 1.15 V (RP2040 datasheet, Pico SDK 2.1+).
#define VREG_HIGH_ABOVE_KHZ 133000u
#define VREG_HIGH_MV 1150u
#else
static const uint32_t s_ops_khz[] = {48000, 96000, 120000, 133000, 150000};
#define DEFAULT_OP 4
#define BOOT_FLASH_MAX_KHZ 150000u
#endif
#define OP_COUNT (sizeof(s_ops_khz) / sizeof(s_ops_khz[0]))
_Static_assert(OP_COUNT <= CLOCKS_MAX_OPS, "CLOCKS_MAX_OPS too small");

// A clk_sys change is crossed with the flash at most this fast, whatever the clocks.
#define FLASH_TRANSIT_SCK_KHZ 40000u
// RP2040 operating points above BOOT_FLASH_MAX_KHZ: the flash SCK tried first (divider
// 2 at 150 and 200 MHz, i.e. 75 and 100 MHz SCK), each candidate verified by reading
// VERIFY_BYTES back VERIFY_PASSES times before it is kept.
#define FLASH_OP_SCK_MAX_KHZ 100000u
#define VERIFY_BYTES (64u * 1024u)
#define VERIFY_PASSES 4u
#define VREG_SETTLE_US 1000u    // as the SDK (SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST_DELAY_US)
#define VREG_MAX_MV 1300u       // VREG_VOLTAGE_MAX: the limit without unlocking the regulator

static unsigned s_current_op = DEFAULT_OP;
static uint32_t s_boot_vreg_mv;
#if PICO_RP2040
static flash_clock_t s_op_flash[OP_COUNT];   // the flash setting chosen for 150 / 200 MHz
static bool s_op_flash_known[OP_COUNT];
#endif

static uint32_t vsel_to_mv(uint32_t vsel) {
#if PICO_RP2040
    return vsel <= 5u ? 800u : 800u + (vsel - 5u) * 50u;
#else
    return vsel <= 15u ? 550u + vsel * 50u : 0u;   // 0: above the normal range
#endif
}

static enum vreg_voltage mv_to_vsel(uint32_t mv) {
    if (mv > VREG_MAX_MV) mv = VREG_MAX_MV;
    if (mv < 850u) mv = 850u;
    return (enum vreg_voltage)((mv - 550u) / 50u);   // same encoding on both chips here
}

static void set_vreg_mv(uint32_t mv) {
    enum vreg_voltage v = mv_to_vsel(mv);
    if (v == vreg_get_voltage()) return;
    bool raising = v > vreg_get_voltage();
    vreg_set_voltage(v);
    if (raising) busy_wait_us(VREG_SETTLE_US);
}

uint32_t clocks_boot_vreg_mv(void) { return s_boot_vreg_mv; }
uint32_t clocks_vreg_mv(void) { return vsel_to_mv(vreg_get_voltage()); }

void clocks_init(void) {
    s_boot_vreg_mv = clocks_vreg_mv();
    set_vreg_mv(vsel_to_mv(VREG_VOLTAGE_DEFAULT));
    flash_clock_init(clock_get_hz(clk_sys) / 1000u);
}

unsigned clocks_op_count(void) { return OP_COUNT; }
uint32_t clocks_op_khz(unsigned op) { return op < OP_COUNT ? s_ops_khz[op] : 0; }
unsigned clocks_default_op(void) { return DEFAULT_OP; }
unsigned clocks_current_op(void) { return s_current_op; }

uint32_t clocks_op_vreg_mv(unsigned op) {
#if PICO_RP2040
    if (op < OP_COUNT && s_ops_khz[op] > VREG_HIGH_ABOVE_KHZ) return VREG_HIGH_MV;
#endif
    (void)op;
    return vsel_to_mv(VREG_VOLTAGE_DEFAULT);
}

uint32_t clocks_attainable_khz(uint32_t khz, uint32_t max_off_khz) {
    uint vco, pd1, pd2;
    for (uint32_t off = 0; off <= max_off_khz; off += 1000u) {
        if (check_sys_clock_khz(khz + off, &vco, &pd1, &pd2)) return khz + off;
        if (off && off < khz && check_sys_clock_khz(khz - off, &vco, &pd1, &pd2)) return khz - off;
    }
    return 0;
}

// clk_sys change that keeps the flash readable all the way:
//   1. raise the core voltage first if the new point needs more,
//   2. slow the flash down to a divider that is safe at both clocks,
//   3. switch the PLL,
//   4. apply the flash setting for the new clock (NULL: the boot setting),
//   5. lower the voltage last.
// Steps 2 and 4 are skipped when the flash is in its boot setting and both clocks are
// within the range that setting is proven for - every RP2350 operating point and the
// RP2040 ones up to 133 MHz switch exactly as before.
static bool switch_clock(uint32_t khz, uint32_t vreg_mv, const flash_clock_t *flash) {
    uint vco, pd1, pd2;
    if (!check_sys_clock_khz(khz, &vco, &pd1, &pd2)) return false;
    uint32_t cur_khz = clock_get_hz(clk_sys) / 1000u;
    if (mv_to_vsel(vreg_mv) > vreg_get_voltage()) set_vreg_mv(vreg_mv);

    bool transit = flash || !flash_clock_is_boot() || khz > BOOT_FLASH_MAX_KHZ || cur_khz > BOOT_FLASH_MAX_KHZ;
    if (transit) {
#if !PICO_RP2040
        xip_cache_clean_all();   // no dirty PSRAM line may be written back mid-change
#endif
        uint16_t d = flash_clock_div_for(khz > cur_khz ? khz : cur_khz, FLASH_TRANSIT_SCK_KHZ);
        flash_clock_t now;
        flash_clock_get(&now);
        if (d > now.div) flash_clock_set_div(d);
    }
    set_sys_clock_pll(vco, pd1, pd2);
    if (transit) {
        if (flash) flash_clock_set(flash);
        else flash_clock_restore_boot();
    }
    if (mv_to_vsel(vreg_mv) < vreg_get_voltage()) set_vreg_mv(vreg_mv);
    led_reclock();
    return true;
}

#if PICO_RP2040
// 150 and 200 MHz: cross at a slow divider, then keep the fastest candidate that reads
// the flash back correctly. The first choice is remembered, so every switch to the same
// point ends up with the same setting (or a slower one if it stops verifying).
static bool switch_fast_op(unsigned op) {
    const uint32_t khz = s_ops_khz[op];
    const uint32_t ref = flash_clock_checksum(VERIFY_BYTES);   // in the current, working setting
    flash_clock_t slow;
    flash_clock_for(khz, FLASH_TRANSIT_SCK_KHZ, &slow);
    if (!switch_clock(khz, clocks_op_vreg_mv(op), &slow)) return false;

    if (s_op_flash_known[op] && flash_clock_try(&s_op_flash[op], ref, VERIFY_BYTES, VERIFY_PASSES)) return true;
    const flash_clock_t *b = flash_clock_boot();
    flash_clock_t cand[3];
    flash_clock_for(khz, FLASH_OP_SCK_MAX_KHZ, &cand[0]);
    cand[1] = cand[0];
    cand[1].rxdelay = (uint8_t)(b->rxdelay + 1u);
    cand[2] = cand[0];
    cand[2].div = (uint16_t)(cand[0].div + 2u);
    for (unsigned i = 0; i < 3; i++) {
        if (cand[i].div >= slow.div) break;
        if (flash_clock_try(&cand[i], ref, VERIFY_BYTES, VERIFY_PASSES)) {
            s_op_flash[op] = cand[i];
            s_op_flash_known[op] = true;
            return true;
        }
    }
    s_op_flash[op] = slow;   // nothing faster verified: stay slow (still recorded per point)
    s_op_flash_known[op] = true;
    return true;
}
#endif

bool clocks_set_op(unsigned op) {
    if (op >= OP_COUNT) return false;
    bool ok;
#if PICO_RP2040
    if (s_ops_khz[op] > BOOT_FLASH_MAX_KHZ) ok = switch_fast_op(op);
    else
#endif
    ok = switch_clock(s_ops_khz[op], clocks_op_vreg_mv(op), NULL);
    if (!ok) return false;
    s_current_op = op;
    return true;
}

bool clocks_set_custom(uint32_t khz, uint32_t vreg_mv, uint32_t flash_sck_max_khz, flash_clock_t *flash_out) {
    flash_clock_t fc;
    flash_clock_for(khz, flash_sck_max_khz, &fc);
    if (!switch_clock(khz, vreg_mv, &fc)) return false;
    s_current_op = CLOCKS_OP_NONE;
    if (flash_out) *flash_out = fc;
    return true;
}

static void read_pll(pll_hw_t *pll, uint32_t xosc_khz, pll_readback_t *out) {
    out->refdiv = (pll->cs & PLL_CS_REFDIV_BITS) >> PLL_CS_REFDIV_LSB;
    out->locked = (pll->cs & PLL_CS_LOCK_BITS) != 0;
    out->fbdiv = (pll->fbdiv_int & PLL_FBDIV_INT_BITS) >> PLL_FBDIV_INT_LSB;
    out->postdiv1 = (pll->prim & PLL_PRIM_POSTDIV1_BITS) >> PLL_PRIM_POSTDIV1_LSB;
    out->postdiv2 = (pll->prim & PLL_PRIM_POSTDIV2_BITS) >> PLL_PRIM_POSTDIV2_LSB;
    out->vco_khz = out->refdiv ? xosc_khz / out->refdiv * out->fbdiv : 0;
    uint32_t pd = out->postdiv1 * out->postdiv2;
    out->out_khz = pd ? out->vco_khz / pd : 0;
}

void clocks_readback(clock_readback_t *out) {
    memset(out, 0, sizeof(*out));
    out->requested_khz = s_current_op < OP_COUNT ? s_ops_khz[s_current_op] : clock_get_hz(clk_sys) / 1000u;
    out->xosc_khz = XOSC_HZ / 1000;
    out->sys_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    out->ref_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_REF);
    out->peri_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI);
    out->usb_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
    out->adc_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC);
#if PICO_RP2040
    out->aux_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_RTC);
#else
    out->aux_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX);
#endif
    read_pll(pll_sys_hw, out->xosc_khz, &out->sys_pll);
    read_pll(pll_usb_hw, out->xosc_khz, &out->usb_pll);

    out->sys_ctrl = clocks_hw->clk[clk_sys].ctrl;
    out->sys_div = clocks_hw->clk[clk_sys].div;
    out->ref_ctrl = clocks_hw->clk[clk_ref].ctrl;
    out->ref_div = clocks_hw->clk[clk_ref].div;
    out->peri_ctrl = clocks_hw->clk[clk_peri].ctrl;
    out->peri_div = clocks_hw->clk[clk_peri].div;
    out->usb_ctrl = clocks_hw->clk[clk_usb].ctrl;
    out->usb_div = clocks_hw->clk[clk_usb].div;
    out->adc_ctrl = clocks_hw->clk[clk_adc].ctrl;
    out->adc_div = clocks_hw->clk[clk_adc].div;

    out->vreg_mv = clocks_vreg_mv();
#if PICO_RP2040
    out->flash_if_reg = ssi_hw->baudr;
    out->flash_if_div = ssi_hw->baudr;
    out->flash_if_rxdelay = ssi_hw->rx_sample_dly;
#else
    out->flash_if_reg = qmi_hw->m[0].timing;
    out->flash_if_div = (out->flash_if_reg & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    out->flash_if_rxdelay = (out->flash_if_reg & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB;
#endif
}

// Source names follow the datasheet mux values. Only what the SDK can select for
// clk_sys / clk_ref is decoded; everything else is also reported raw.
const char *clocks_src_name_sys(uint32_t ctrl) {
    if ((ctrl & CLOCKS_CLK_SYS_CTRL_SRC_BITS) == CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF) return "clk_ref";
    switch ((ctrl & CLOCKS_CLK_SYS_CTRL_AUXSRC_BITS) >> CLOCKS_CLK_SYS_CTRL_AUXSRC_LSB) {
    case 0: return "pll_sys";
    case 1: return "pll_usb";
    case 2: return "rosc";
    case 3: return "xosc";
    default: return "gpin/other";
    }
}

const char *clocks_src_name_ref(uint32_t ctrl) {
    switch ((ctrl & CLOCKS_CLK_REF_CTRL_SRC_BITS) >> CLOCKS_CLK_REF_CTRL_SRC_LSB) {
    case 0: return "rosc";
    case 1: return "aux";
    case 2: return "xosc";
#if !PICO_RP2040
    case 3: return "lposc";
#endif
    default: return "?";
    }
}

// clk_peri, clk_usb and clk_adc share the auxsrc encoding for their first entries.
const char *clocks_src_name_aux(const char *clock, uint32_t ctrl) {
    uint32_t aux = (ctrl >> 5) & 0x7;
    bool is_peri = clock[0] == 'p';
    if (is_peri) {
        switch (aux) {
        case 0: return "clk_sys";
        case 1: return "pll_sys";
        case 2: return "pll_usb";
        case 3: return "rosc";
        case 4: return "xosc";
        default: return "gpin";
        }
    }
    switch (aux) {
    case 0: return "pll_usb";
    case 1: return "pll_sys";
    case 2: return "rosc";
    case 3: return "xosc";
    default: return "gpin";
    }
}
