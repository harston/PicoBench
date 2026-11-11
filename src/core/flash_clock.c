#include "core/flash_clock.h"

#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#if PICO_RP2040
#include "hardware/structs/ssi.h"
#else
#include "hardware/structs/qmi.h"
#endif

static flash_clock_t s_boot;
static uint32_t s_boot_khz;

// A couple of thousand cycles without a memory access, so that a flash access still in
// flight when the caller branched into SRAM has finished before the interface changes.
static __force_inline void settle(void) {
    for (uint32_t i = 0; i < 1024u; i++) __asm volatile("nop");
}

static __force_inline uint32_t sum_words(uint32_t bytes) {
    const volatile uint32_t *w = (const volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    uint32_t sum = 0x1234u;
    for (uint32_t i = 0; i < bytes / 4u; i++) {
        sum = (sum << 1) | (sum >> 31);
        sum += w[i];
    }
    return sum;
}

// ---- raw register access (everything called from SRAM is force-inlined) -----------

#if PICO_RP2040

typedef struct {
    uint32_t baudr, rx;
} raw_t;

static uint32_t s_boot_baudr, s_boot_rx;

static __force_inline void raw_read(raw_t *r) {
    r->baudr = ssi_hw->baudr;
    r->rx = ssi_hw->rx_sample_dly;
}

static __force_inline void raw_write(const raw_t *r) {
    settle();
    ssi_hw->ssienr = 0;
    ssi_hw->baudr = r->baudr;
    ssi_hw->rx_sample_dly = r->rx;
    ssi_hw->ssienr = 1;
}

static void to_raw(const flash_clock_t *c, raw_t *r) {
    r->baudr = c->div;
    r->rx = c->rxdelay;
}

static void from_raw(const raw_t *r, flash_clock_t *c) {
    c->div = (uint16_t)r->baudr;
    c->rxdelay = (uint8_t)r->rx;
    c->min_deselect = 0;
}

#else

typedef struct {
    uint32_t timing;
} raw_t;

static uint32_t s_boot_timing;

#define FC_FIELDS (QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS | QMI_M0_TIMING_MIN_DESELECT_BITS)

static __force_inline void raw_read(raw_t *r) {
    r->timing = qmi_hw->m[0].timing;
}

static __force_inline void raw_write(const raw_t *r) {
    settle();
    qmi_hw->m[0].timing = r->timing;
}

static void to_raw(const flash_clock_t *c, raw_t *r) {
    uint32_t div = c->div >= 256u ? 0u : c->div;   // 256 is encoded as 0
    r->timing = (qmi_hw->m[0].timing & ~FC_FIELDS) |
                (div << QMI_M0_TIMING_CLKDIV_LSB) |
                ((uint32_t)c->rxdelay << QMI_M0_TIMING_RXDELAY_LSB) |
                ((uint32_t)c->min_deselect << QMI_M0_TIMING_MIN_DESELECT_LSB);
}

static void from_raw(const raw_t *r, flash_clock_t *c) {
    uint32_t div = (r->timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    c->div = (uint16_t)(div ? div : 256u);
    c->rxdelay = (uint8_t)((r->timing & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB);
    c->min_deselect = (uint8_t)((r->timing & QMI_M0_TIMING_MIN_DESELECT_BITS) >>
                                QMI_M0_TIMING_MIN_DESELECT_LSB);
}

#endif

// ---- SRAM-resident parts ---------------------------------------------------------------

static void __no_inline_not_in_flash_func(write_sram)(const raw_t *r) {
    uint32_t irq = save_and_disable_interrupts();
    raw_write(r);
    restore_interrupts(irq);
}

static uint32_t __no_inline_not_in_flash_func(checksum_sram)(uint32_t bytes) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t sum = sum_words(bytes);
    restore_interrupts(irq);
    return sum;
}

static bool __no_inline_not_in_flash_func(try_sram)(const raw_t *next, const raw_t *prev, uint32_t ref,
                                                    uint32_t bytes, uint32_t passes) {
    uint32_t irq = save_and_disable_interrupts();
    raw_write(next);
    bool ok = true;
    for (uint32_t p = 0; p < passes && ok; p++) ok = sum_words(bytes) == ref;
    if (!ok) raw_write(prev);
    restore_interrupts(irq);
    return ok;
}

// ---- API ---------------------------------------------------------------------------------

void flash_clock_init(uint32_t clk_khz) {
    raw_t r;
    raw_read(&r);
#if PICO_RP2040
    s_boot_baudr = r.baudr;
    s_boot_rx = r.rx;
#else
    s_boot_timing = r.timing;
#endif
    from_raw(&r, &s_boot);
    s_boot_khz = clk_khz;
}

const flash_clock_t *flash_clock_boot(void) {
    return &s_boot;
}

bool flash_clock_is_boot(void) {
    raw_t r;
    raw_read(&r);
#if PICO_RP2040
    return r.baudr == s_boot_baudr && r.rx == s_boot_rx;
#else
    return r.timing == s_boot_timing;
#endif
}

void flash_clock_get(flash_clock_t *out) {
    raw_t r;
    raw_read(&r);
    from_raw(&r, out);
}

void flash_clock_set(const flash_clock_t *c) {
    raw_t r;
    to_raw(c, &r);
    write_sram(&r);
}

void flash_clock_restore_boot(void) {
    raw_t r;
#if PICO_RP2040
    r.baudr = s_boot_baudr;
    r.rx = s_boot_rx;
#else
    r.timing = s_boot_timing;
#endif
    write_sram(&r);
}

void flash_clock_set_div(uint16_t div) {
    flash_clock_t c;
    flash_clock_get(&c);
    c.div = div;
    flash_clock_set(&c);
}

uint16_t flash_clock_div_for(uint32_t clk_khz, uint32_t sck_max_khz) {
    uint32_t d = sck_max_khz ? (clk_khz + sck_max_khz - 1u) / sck_max_khz : 255u;
#if PICO_RP2040
    if (d < 2u) d = 2u;
    d = (d + 1u) & ~1u;     // BAUDR ignores bit 0
#else
    if (d < 1u) d = 1u;
#endif
    return (uint16_t)(d > 254u ? 254u : d);
}

void flash_clock_for(uint32_t clk_khz, uint32_t sck_max_khz, flash_clock_t *out) {
    out->div = flash_clock_div_for(clk_khz, sck_max_khz);
#if PICO_RP2040
    // Boot stage 2's 1-cycle delay is what it was designed with at divider 2; at larger
    // dividers it has no effect (boot2_w25q080.S).
    out->rxdelay = s_boot.rxdelay;
    out->min_deselect = 0;
#else
    uint32_t bk = s_boot_khz ? s_boot_khz : clk_khz;
    uint32_t rx = (s_boot.rxdelay * clk_khz + bk / 2u) / bk;          // same ns, rounded
    uint32_t md = (s_boot.min_deselect * clk_khz + bk - 1u) / bk;     // same ns, rounded up
    out->rxdelay = (uint8_t)(rx > 7u ? 7u : rx);
    out->min_deselect = (uint8_t)(md > 31u ? 31u : md);
#endif
}

uint32_t flash_clock_checksum(uint32_t bytes) {
    return checksum_sram(bytes);
}

bool flash_clock_try(const flash_clock_t *c, uint32_t ref, uint32_t bytes, uint32_t passes) {
    raw_t now, next;
    raw_read(&now);
    to_raw(c, &next);
    return try_sram(&next, &now, ref, bytes, passes);
}
