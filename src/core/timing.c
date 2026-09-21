#include "core/timing.h"

#if !PICO_RP2040 && !defined(__riscv)
#include "hardware/regs/m33.h"
#endif

// Written by core 0 during init; both cores' counters have the same read cost.
uint32_t g_timing_overhead;

void timing_init(void) {
#if PICO_RP2040
    systick_hw->csr = 0;
    systick_hw->rvr = TIMING_COUNTER_MASK;
    systick_hw->cvr = 0;
    systick_hw->csr = 0x5;                          // ENABLE | CLKSOURCE=processor clock, no IRQ
#elif defined(__riscv)
    riscv_clear_csr(mcountinhibit, 1u);             // bit 0: CY
#else
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
#endif

    uint32_t best = UINT32_MAX;
    for (int i = 0; i < 64; i++) {
        uint32_t t0 = timing_now();
        uint32_t t1 = timing_now();
        uint32_t e = timing_elapsed(t0, t1);
        if (e < best) best = e;
    }
    g_timing_overhead = best;
}

