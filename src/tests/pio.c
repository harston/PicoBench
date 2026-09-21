/*
 * PIO: every block, every state machine.
 *
 * The programs are assembled at run time with pio_encode_*, so there is no build step
 * and the exact instructions are visible here. No state machine is given any pins, so
 * nothing is driven externally; what is measured is the CPU/PIO interface, which is
 * what a PIO-based cartridge data path would live on:
 *
 *   - how long a word takes to go CPU -> FIFO -> state machine -> FIFO -> CPU,
 *   - what a FIFO write and read cost on their own,
 *   - how fast a state machine can be fed by DMA,
 *   - how long a PIO interrupt takes to become visible to the CPU.
 */

#include "tests/tests.h"

#include <stdio.h>

#include "core/bench.h"
#include "core/guard.h"
#include "core/stats.h"
#include "core/timing.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/sync.h"
#include "results/record.h"

#define SAMPLES 256u

static volatile uint32_t s_sink;

static PIO pio_at(unsigned i) {
#if NUM_PIOS > 2
    return i == 0 ? pio0 : (i == 1 ? pio1 : pio2);
#else
    return i == 0 ? pio0 : pio1;
#endif
}

// ---- Programs -------------------------------------------------------------------------

// Echo: wait for a word, hand it straight back.
//   pull block ; mov isr, osr ; push block ; jmp 0
static uint16_t s_echo[4];
static const pio_program_t s_echo_prog = {.instructions = s_echo, .length = 4, .origin = -1};

// Drain: consume 32 bits per instruction, so the FIFO empties as fast as it is filled.
//   out null, 32   (autopull keeps the OSR fed)
static uint16_t s_drain[1];
static const pio_program_t s_drain_prog = {.instructions = s_drain, .length = 1, .origin = -1};

// Signal: wait for a word, then raise PIO IRQ 0.
//   pull block ; irq nowait 0 ; jmp 0
static uint16_t s_signal[3];
static const pio_program_t s_signal_prog = {.instructions = s_signal, .length = 3, .origin = -1};

static void build_programs(void) {
    s_echo[0] = pio_encode_pull(false, true);
    s_echo[1] = pio_encode_mov(pio_isr, pio_osr);
    s_echo[2] = pio_encode_push(false, true);
    s_echo[3] = pio_encode_jmp(0);

    s_drain[0] = pio_encode_out(pio_null, 32);

    s_signal[0] = pio_encode_pull(false, true);
    s_signal[1] = pio_encode_irq_set(false, 0);
    s_signal[2] = pio_encode_jmp(0);
}

// Loads `prog` into `pio` and starts state machine `sm` at full clock.
static bool start_sm(PIO pio, uint sm, const pio_program_t *prog, int *offset_out, bool autopull) {
    if (!pio_can_add_program(pio, prog)) return false;
    int off = pio_add_program(pio, prog);
    if (off < 0) return false;
    *offset_out = off;
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, off, off + prog->length - 1);
    sm_config_set_clkdiv_int_frac(&c, 1, 0);       // full clk_sys
    if (autopull) sm_config_set_out_shift(&c, false, true, 32);
    pio_sm_init(pio, sm, off, &c);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_set_enabled(pio, sm, true);
    return true;
}

static void stop_sm(PIO pio, uint sm, const pio_program_t *prog, int offset) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    if (offset >= 0) pio_remove_program(pio, prog, (uint)offset);
}

// ---- Inventory: every block and state machine answers -------------------------------------

test_result_t test_pio_inventory(test_ctx_t *ctx) {
    build_programs();
    rec_value(ctx, "pio_blocks", UNIT_COUNT, NUM_PIOS);
    rec_value(ctx, "state_machines", UNIT_COUNT, NUM_PIOS * 4);
    rec_value(ctx, "instruction_memory", UNIT_COUNT, PIO_INSTRUCTION_COUNT);

    uint32_t failures = 0;
    for (unsigned b = 0; b < NUM_PIOS; b++) {
        PIO pio = pio_at(b);
        for (uint sm = 0; sm < 4; sm++) {
            guard_keepalive();
            char key[REC_LABEL_LEN];
            snprintf(key, sizeof(key), "pio%u_sm%u", b, sm);
            int off = -1;
            if (!start_sm(pio, sm, &s_echo_prog, &off, false)) {
                rec_text(ctx, key, "could not load the program (instruction memory in use?)");
                failures++;
                continue;
            }
            // Round-trip one word and check it comes back unchanged.
            const uint32_t magic = 0xA5C30F51u + sm;
            uint32_t irq = save_and_disable_interrupts();
            uint32_t t0 = timing_now();
            pio->txf[sm] = magic;
            uint32_t guard_count = 0;
            while (pio_sm_is_rx_fifo_empty(pio, sm) && ++guard_count < 100000u) {}
            uint32_t t1 = timing_now();
            uint32_t got = guard_count < 100000u ? pio->rxf[sm] : 0;
            restore_interrupts(irq);

            if (got != magic) {
                rec_text(ctx, key, "no answer or wrong data (got 0x%08lx, expected 0x%08lx)",
                         (unsigned long)got, (unsigned long)magic);
                rec_error(ctx, key, off, magic, got);
                failures++;
            } else {
                rec_text(ctx, key, "ok, program at offset %d, round trip %lu cycles", off,
                         (unsigned long)timing_net(t0, t1));
            }
            stop_sm(pio, sm, &s_echo_prog, off);
        }
    }
    rec_value(ctx, "failures", UNIT_COUNT, failures);
    return failures ? TR_FAIL : TR_PASS;
}

// ---- FIFO timing --------------------------------------------------------------------------

test_result_t test_pio_fifo(test_ctx_t *ctx) {
    build_programs();
    PIO pio = pio0;
    const uint sm = 0;
    int off = -1;
    if (!start_sm(pio, sm, &s_echo_prog, &off, false)) return TR_NA;

    // Round trip: CPU -> TX FIFO -> state machine -> RX FIFO -> CPU.
    for (uint32_t i = 0; i < SAMPLES; i++) {
        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = timing_now();
        pio->txf[sm] = i;
        uint32_t spins = 0;
        while (pio_sm_is_rx_fifo_empty(pio, sm) && ++spins < 100000u) {}
        uint32_t v = pio_sm_is_rx_fifo_empty(pio, sm) ? 0 : pio->rxf[sm];
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        g_bench_samples[i] = timing_net(t0, t1);
        s_sink += v;
    }
    stats_t s;
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "round_trip_cpu_pio_cpu", UNIT_CYCLES, 1, &s);
    guard_keepalive();

    // Write cost alone, with the state machine stopped so the FIFO cannot drain.
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    for (uint32_t i = 0; i < 4; i++) {   // FIFO is 4 deep
        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = timing_now();
        pio->txf[sm] = i;
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        g_bench_samples[i] = timing_net(t0, t1);
    }
    stats_from_samples(g_bench_samples, 4, &s);
    rec_stats(ctx, "tx_fifo_write", UNIT_CYCLES, 1, &s);
    rec_value(ctx, "tx_fifo_depth", UNIT_COUNT, 4);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_set_enabled(pio, sm, true);

    // Sustained rate: keep the FIFO fed and count how long a batch takes.
    const uint32_t batch = 1024;
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = timing_now();
    uint32_t sent = 0, received = 0, spins = 0;
    while (received < batch && ++spins < 10000000u) {
        if (sent < batch && !pio_sm_is_tx_fifo_full(pio, sm)) pio->txf[sm] = sent++;
        if (!pio_sm_is_rx_fifo_empty(pio, sm)) { s_sink += pio->rxf[sm]; received++; }
    }
    uint32_t t1 = timing_now();
    restore_interrupts(irq);
    uint32_t cycles = timing_net(t0, t1);
    if (received < batch) rec_error(ctx, "sustained_stalled", sm, batch, received);
    rec_value(ctx, "sustained_words", UNIT_COUNT, received);
    rec_value(ctx, "sustained_cycles", UNIT_CYCLES, cycles);
    rec_value(ctx, "sustained_mcyc_per_word", UNIT_COUNT, (int64_t)((uint64_t)cycles * 1000u / batch));

    stop_sm(pio, sm, &s_echo_prog, off);
    rec_text(ctx, "note", "the state machine runs at clk_sys; a word costs the round trip through "
                          "both FIFOs, not just the instruction time");
    return TR_PASS;
}

// ---- PIO interrupt to CPU ---------------------------------------------------------------------

test_result_t test_pio_irq(test_ctx_t *ctx) {
    build_programs();
    PIO pio = pio0;
    const uint sm = 0;
    int off = -1;
    if (!start_sm(pio, sm, &s_signal_prog, &off, false)) return TR_NA;

    for (uint32_t i = 0; i < SAMPLES; i++) {
        pio_interrupt_clear(pio, 0);
        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = timing_now();
        pio->txf[sm] = i;                       // makes the state machine raise IRQ 0
        uint32_t spins = 0;
        while (!pio_interrupt_get(pio, 0) && ++spins < 100000u) {}
        uint32_t t1 = timing_now();
        restore_interrupts(irq);
        g_bench_samples[i] = timing_net(t0, t1);
    }
    stats_t s;
    stats_from_samples(g_bench_samples, SAMPLES, &s);
    rec_stats(ctx, "pio_irq_flag_visible", UNIT_CYCLES, 1, &s);
    pio_interrupt_clear(pio, 0);
    stop_sm(pio, sm, &s_signal_prog, off);
    rec_text(ctx, "note", "time from the trigger word to the IRQ flag being readable by the CPU, "
                          "polled; an actual interrupt handler adds its own entry cost (see irq.latency)");
    return TR_PASS;
}

// ---- DMA feeding a state machine -----------------------------------------------------------------

test_result_t test_pio_dma(test_ctx_t *ctx) {
    build_programs();
    if (bench_ram_size() < 64u * 1024u) return TR_NA;
    PIO pio = pio0;
    const uint sm = 0;
    int off = -1;
    if (!start_sm(pio, sm, &s_drain_prog, &off, true)) return TR_NA;

    int ch = dma_claim_unused_channel(false);
    if (ch < 0) {
        stop_sm(pio, sm, &s_drain_prog, off);
        return TR_NA;
    }
    const uint32_t words = 16384;
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));

    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = timing_now();
    dma_channel_configure(ch, &c, &pio->txf[sm], bench_ram(), words, true);
    dma_channel_wait_for_finish_blocking(ch);
    uint32_t t1 = timing_now();
    restore_interrupts(irq);
    uint32_t cycles = timing_net(t0, t1);

    rec_value(ctx, "dma_to_pio_words", UNIT_COUNT, words);
    rec_value(ctx, "dma_to_pio_cycles", UNIT_CYCLES, cycles);
    rec_value(ctx, "dma_to_pio_mcyc_per_word", UNIT_COUNT, (int64_t)((uint64_t)cycles * 1000u / words));
    rec_value(ctx, "dma_to_pio_rate", UNIT_KBPS,
              cycles ? (int64_t)((uint64_t)words * 4u * ctx->clk_khz / cycles) : 0);

    dma_channel_unclaim(ch);
    stop_sm(pio, sm, &s_drain_prog, off);
    rec_text(ctx, "note", "the state machine consumes 32 bits per instruction with autopull, so "
                          "this is the DMA-to-PIO path at full state machine speed");
    return TR_PASS;
}
