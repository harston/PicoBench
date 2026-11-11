#include "core/sequencer.h"

#include <string.h>

#include "pico/time.h"
#include "core/clocks_ctl.h"
#include "core/guard.h"
#include "core/led.h"

static uint8_t s_chip = CHIP_RP2040;

void sequencer_set_chip(uint8_t chip_bit) { s_chip = chip_bit; }
uint8_t sequencer_chip(void) { return s_chip; }

const test_desc_t *registry_find(uint16_t id) {
    for (unsigned i = 0; i < g_test_count; i++) {
        if (g_tests[i].id == id) return &g_tests[i];
    }
    return NULL;
}

const char *subsys_name(subsys_t s) {
    static const char *const names[SUBSYS_COUNT] = {
        "CORE", "CLOCKS", "CPU", "SRAM", "FLASH", "PSRAM", "DMA",
        "PIO", "GPIO", "MULTICORE", "IRQ", "USB", "CRYPTO", "ADC",
    };
    return s < SUBSYS_COUNT ? names[s] : "?";
}

static unsigned sweep_test_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < g_test_count; i++) {
        if (g_tests[i].flags & TF_SWEEP) n++;
    }
    return n;
}

uint32_t sequencer_stage_count(void) {
    return g_test_count + (clocks_op_count() - 1) * sweep_test_count();
}

bool sequencer_stage(uint32_t stage, unsigned *op, const test_desc_t **test) {
    if (stage < g_test_count) {
        *op = clocks_default_op();
        *test = &g_tests[stage];
        return true;
    }
    stage -= g_test_count;
    unsigned per_op = sweep_test_count();
    if (per_op == 0) return false;
    unsigned sweep_idx = stage / per_op;       // index among non-default ops
    unsigned in_op = stage % per_op;
    if (sweep_idx >= clocks_op_count() - 1) return false;
    unsigned o = sweep_idx >= clocks_default_op() ? sweep_idx + 1 : sweep_idx;
    for (unsigned i = 0; i < g_test_count; i++) {
        if (!(g_tests[i].flags & TF_SWEEP)) continue;
        if (in_op-- == 0) {
            *op = o;
            *test = &g_tests[i];
            return true;
        }
    }
    return false;
}

static bool switch_op(unsigned op) {
    if (clocks_current_op() == op) return true;
    guard_keepalive();
    return clocks_set_op(op);
}

static void log_oppoint_once(unsigned op, bool ok) {
    run_state_t *st = record_run_state();
    if (st->oppoints_logged & (1u << op)) return;
    clock_readback_t rb;
    clocks_readback(&rb);
    rec_oppoint_t r;
    memset(&r, 0, sizeof(r));
    r.requested_khz = clocks_op_khz(op);
    r.sys_khz = rb.sys_khz;
    r.ref_khz = rb.ref_khz;
    r.peri_khz = rb.peri_khz;
    r.usb_khz = rb.usb_khz;
    r.adc_khz = rb.adc_khz;
    r.aux_khz = rb.aux_khz;
    r.pll_sys_refdiv = rb.sys_pll.refdiv;
    r.pll_sys_fbdiv = rb.sys_pll.fbdiv;
    r.pll_sys_pd1 = rb.sys_pll.postdiv1;
    r.pll_sys_pd2 = rb.sys_pll.postdiv2;
    r.pll_usb_refdiv = rb.usb_pll.refdiv;
    r.pll_usb_fbdiv = rb.usb_pll.fbdiv;
    r.pll_usb_pd1 = rb.usb_pll.postdiv1;
    r.pll_usb_pd2 = rb.usb_pll.postdiv2;
    r.flash_if_reg = rb.flash_if_reg;
    r.flash_if_div = rb.flash_if_div;
    r.flash_if_rxdelay = rb.flash_if_rxdelay;
    r.vreg_mv = rb.vreg_mv;
    r.led_blink_mhz = led_actual_freq_mhz();
    r.ok = ok;
    rec_oppoint((uint8_t)op, &r);
    st->oppoints_logged |= 1u << op;
    record_commit_state();
}

void sequencer_run(uint32_t skip_stage) {
    run_state_t *st = record_run_state();
    const uint32_t total = sequencer_stage_count();

    for (uint32_t stage = st->next_stage; stage < total; stage++) {
        unsigned op;
        const test_desc_t *t;
        if (!sequencer_stage(stage, &op, &t)) break;

        if (stage == skip_stage) {
            // Already recorded as REC_CRASH by main(); mark the test itself too.
            rec_test_end(t->id, (uint8_t)op, TR_CRASHED, 0, 0);
        } else if (!(t->chip_mask & s_chip)) {
            rec_test_end(t->id, (uint8_t)op, TR_NA, 0, 0);
        } else {
            guard_stage_begin(stage);
            bool op_ok = switch_op(op);
            log_oppoint_once(op, op_ok);
            if (!op_ok) {
                rec_test_end(t->id, (uint8_t)op, TR_SKIPPED, 0, 0);
            } else {
                test_ctx_t ctx = {
                    .test = t,
                    .op = (uint8_t)op,
                    .clk_khz = clocks_op_khz(op),
                };
                uint64_t t0 = time_us_64();
                test_result_t res = t->fn(&ctx);
                uint32_t dur = (uint32_t)(time_us_64() - t0);
                if (res == TR_PASS && ctx.errors) res = TR_FAIL;
                rec_test_end(t->id, (uint8_t)op, res, dur, ctx.errors);
            }
            guard_stage_end();
        }
        st->next_stage = stage + 1;
        record_commit_state();
    }

    // Leave the chip at the default operating point.
    switch_op(clocks_default_op());
}
