#include "core/clocks_ctl.h"

#include <string.h>

#include "core/led.h"
#include "core/timing.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/regs/pll.h"
#if PICO_RP2040
#include "hardware/structs/ssi.h"
#else
#include "hardware/structs/qmi.h"
#endif

#if PICO_RP2040
static const uint32_t s_ops_khz[CLOCKS_MAX_OPS] = {48000, 96000, 120000, 125000, 133000};
#define DEFAULT_OP 3
#else
static const uint32_t s_ops_khz[CLOCKS_MAX_OPS] = {48000, 96000, 120000, 133000, 150000};
#define DEFAULT_OP 4
#endif

static unsigned s_current_op = DEFAULT_OP;

unsigned clocks_op_count(void) { return CLOCKS_MAX_OPS; }
uint32_t clocks_op_khz(unsigned op) { return op < CLOCKS_MAX_OPS ? s_ops_khz[op] : 0; }
unsigned clocks_default_op(void) { return DEFAULT_OP; }
unsigned clocks_current_op(void) { return s_current_op; }

bool clocks_set_op(unsigned op) {
    if (op >= CLOCKS_MAX_OPS) return false;
    if (!set_sys_clock_khz(s_ops_khz[op], false)) return false;
    s_current_op = op;
    led_reclock();
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
    out->requested_khz = s_ops_khz[s_current_op];
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

#if PICO_RP2040
    out->flash_if_reg = ssi_hw->baudr;
    out->flash_if_div = ssi_hw->baudr;
#else
    out->flash_if_reg = qmi_hw->m[0].timing;
    out->flash_if_div = (out->flash_if_reg & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
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
