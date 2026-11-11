# PicoBench board comparison

Three boards measured with PicoBench: a Raspberry Pi Pico (RP2040) clone, a Pico 2
(RP2350A) clone and a WeAct RP2350B Core with PSRAM. The comparison covers CPU, SRAM, flash/XIP in
every read mode the chips offer (serial, dual, quad, with and without the command byte,
three flash clocks), PSRAM, the Atari cartridge "hot path", peripherals, and how all of
it scales with the system clock from 48 to 200 MHz. A last section shows how far each chip
goes outside its specification (overclocking run).

An HTML version with charts is in [board-comparison.html](board-comparison.html).

All timings are **internal**: CPU and bus cycles counted by the chip itself (SysTick on
the M0+, DWT CYCCNT on the M33, `mcycle` on Hazard3). Electrical timing (rise time, PCB
propagation, input thresholds) is not measured. Latencies are reported as p50 / p99.9 /
max over the stated samples; "cyc" = clk_sys cycles.

## The boards

| | **RP2040** (purple Pico clone) | **RP2350A** (black Pico 2 clone) | **RP2350B** (WeAct Core + PSRAM) |
|---|---|---|---|
| Silicon | RP2040-B2 | RP2350-A2 | RP2350-A3 |
| Package / GPIO | QFN56 / 30 | QFN60 / 30 | QFN80 / 48 |
| Cores measured | 2x Cortex-M0+ | 2x Cortex-M33 or 2x Hazard3 (RISC-V) | 2x Cortex-M33 or 2x Hazard3 |
| SRAM / DMA channels / PIO blocks | 264 KB / 12 / 2 | 520 KB / 16 / 3 | 520 KB / 16 / 3 |
| Flash | 16 MB **Boya** (JEDEC 68 40 18, SFDP 1.6) | 16 MB **Boya** (68 40 18, older revision, SFDP 1.0) | 16 MB **XMC** (20 40 18, SFDP 1.6) |
| Flash read mode at boot | quad, **continuous read** (boot stage 2), SCK = clk_sys / 2 | **dual** 1-2-2 (0xBB) with command byte, QE bit clear, SCK = clk_sys / 3 | **quad** 1-4-4 (0xEB) with command byte, SCK = clk_sys / 3 |
| PSRAM | none | none | AP Memory APS6404, 8 MB, CS = GPIO0 |
| Clocks tested (default) | 48 / 96 / 120 / **125** / 133 / 150* / 200* MHz | 48 / 96 / 120 / 133 / **150** MHz | 48 / 96 / 120 / 133 / **150** MHz |
| Boot ROM version | 3 | 2 | 4 |

\* At 1.15 V core supply. The RP2040 has been rated up to 200 MHz at 1.15 V since Pico
SDK 2.1; every other point runs at the default 1.10 V.

Except in the last section, every operating point is within the chip's rated maximum, and
the real clock and core voltage were read back at each point.

**Data**: PicoBench firmware 0.0.1+9f87c89, one board of each kind (plus a second
RP2040 board for the last runs), room temperature.

- **Main runs.** RP2040: build 7e194a34 (2026-11-06). RP2350A and RP2350B on the M33:
  build 425991b6 (2026-11-06). RP2350B on Hazard3: build 7f0ad49e (2026-11-06).
- **RP2350A on Hazard3.** Build 906d2ad9 (2026-10-02), from before the cold-miss
  measurement fix, so its flash-miss figure is not used. On everything else it matches
  the RP2350B Hazard3 run to within 1-2 cycles.
- **Limits within specification.** Build b6f94055 (RP2040: 150 and 200 MHz) and 80ea42b1
  (RP2350 M33), 2026-11-06. These runs measured the fastest flash and PSRAM clock at
  every operating point.
- **Overclocking.** RP2040: builds ec2091cb, 0a37c20c and c17c8e33. RP2350 M33:
  35e0ef24, ea1013ce and 65e684f0. Hazard3: 6e9e2e7d, 5ac2d91a and 39a7cf7a.
- **Corrected measurements, 2026-11-07.** RP2040: builds fda610e7 and 7f5fd1b7. RP2350B
  on the M33: builds c4d1438f and e987cf0f. RP2350A on the M33: build e987cf0f. The PSRAM
  and flash sampling loops moved to SRAM, and the flash DMA in the QMI conflict test now
  reads the uncached alias. The RP2040 overclocking run with the flash probe used build
  fa092ba5.
- **Phase-swept SRAM contention and full-walk PSRAM chase, 2026-11-11.** RP2040: builds
  912a6bda and bea4ca63, on a second purple board of the same type (its cycle counts
  match the first board's in 931 of 999 figures). RP2350A and RP2350B on the M33: build
  d952e340. All other figures of these runs match the earlier runs.

Every run outside the overclocking section completed all stages with OVERALL PASS and no
errors.

## Headline numbers

At each board's default clock (RP2040 125 MHz, RP2350 150 MHz). Where the RP2350 has two
figures, they are Cortex-M33 / Hazard3.

| | RP2040 | RP2350A | RP2350B |
|---|---|---|---|
| Cartridge hot path, table and code in SRAM (p50 / max) | 33 / 40 cyc = 264 / 320 ns | 20 / 23 cyc = **133 / 153 ns**; H3 18 / 19 cyc = 120 / 127 ns | same as RP2350A |
| Flash cache miss, boot read mode | 53 cyc = **0.42 us** | 173 cyc = **1.15 us** | 113 cyc = **0.75 us** |
| Flash cache miss, best setting tested | 53 cyc at 200 MHz = **0.27 us** (SCK 100 MHz)\*\* | 34 cyc at 133 MHz = **0.26 us** (quad, continuous read, SCK 133 MHz); 62 cyc = 0.41 us at 150 MHz | same as RP2350A |
| Uncached flash read, boot mode / best tested | 9.6 / 15.3 MB/s (200 MHz) | 7.2 / 24.9 MB/s | 11.0 / 24.9 MB/s |
| SRAM dependent load | 3.52 cyc (28 ns) | 2.26 cyc (15 ns); H3 3.01 cyc (20 ns) | same as RP2350A |
| SRAM sequential 32-bit read | 142 MB/s | 239 MB/s; H3 265 MB/s | same as RP2350A |
| DMA copy, 16 KB SRAM to SRAM | 496 MB/s | 597 MB/s | 597 MB/s |
| SRAM read while a DMA copy runs in the same banks | +36 % | **+4 %** (other bank group: 0 %) | same as RP2350A |
| Software interrupt entry (p50 / max) | 25 / 428 cyc | 25 / 1044-1310 cyc\*\*\*; H3 45 / 698 | 25 / 624-818 cyc\*\*\*; H3 45 / 383 |
| ARM (Thumb) interpreter, cycles per guest instruction | 42.3 | 28.2; H3 24.5 | 28.2; H3 24.4 |
| PSRAM uncached read / cache miss | - | - | 55 / 66 cyc (0.37 / 0.44 us); best 33 cyc = **0.25 us** (133 MHz, divisor 1) |
| Highest clock in the overclocking run (1.30 V, outside spec) | 420 MHz confirmed, 432-450 passed | ~320 MHz at any voltage | 412 MHz (M33) / 420 MHz (Hazard3) confirmed |

\*\* The miss itself is measured at the default clock only. The cycle count does not
change with the clock at a fixed divider: the hot path's cold flash fetch stays at 84
cycles at 200 MHz.

\*\*\* The maximum is the handler's first run, fetched cold from flash. It changes with
the code layout: 1044 or 1310 cycles on the RP2350A and 624 or 818 on the RP2350B in the
M33 builds from results_8 to results_15. The ratio between the boards (dual vs quad
flash) stays 1.6x.

In clock cycles the two RP2350 boards are **identical** in everything that does not
touch flash. Every difference between them comes from the flash read mode the boot ROM
leaves behind, and that turns out to be configurable (see below).

## CPU

Cycles per operation, independent of the clock (identical at all five operating points
and on both RP2350 boards).

| | Cortex-M0+ (RP2040) | Cortex-M33 (RP2350) | Hazard3 (RP2350) |
|---|---|---|---|
| add / and-or / shift, 32-bit | 1.00 | 1.00 | **0.88** |
| multiply, 32-bit (dependent chain) | 1.00 | 1.88 | **0.88** |
| divide, 32-bit | 33.0 (no divide instruction) | **4.0** | 19.9 |
| empty loop iteration | 4.06 | 3.04 | 3.04 |
| predictable branch, per iteration | 7.0 | 7.0 | **6.0** |
| data-dependent branches vs predictable (same loop) | 1.71x | 1.43x | **1.33x** |
| memcpy 4 KB, cycles per byte | 0.83 | 0.89 | **0.63** |
| memset 4 KB, cycles per byte | 0.52 | 0.52 | **0.38** |
| CRC-32 in software (nibble table), cycles per byte | 19.0 | 15.5 | **14.0** |
| random table lookup in SRAM, cycles per lookup | 15.2 | **11.1** | 12.1 |

Hazard3 wins integer arithmetic, branches and block copies by 12-30 %. The M33 wins
division (5x) and dependent loads (lookups, pointer chasing).

## SRAM

| | RP2040 | RP2350 M33 | RP2350 Hazard3 |
|---|---|---|---|
| Dependent load (pointer chase, 4-64 KB) | 3.52 cyc | **2.26 cyc** | 3.01 cyc |
| Sequential read, 32-bit, at default clock | 142 MB/s | 239 MB/s | **265 MB/s** |
| Sequential write, 32-bit, at default clock | 188 MB/s | 367 MB/s | **473 MB/s** |
| Bank conflicts (stride 4-128 bytes) | none measurable | none measurable | none measurable |
| CPU read while DMA copies in the same bank group | +35-36 %\* | **+4 %**\* | not measured |
| CPU read while DMA copies in the other bank group | (one group only) | **0 %**\* | not measured |

\* A 4 KB sequential CPU read while one DMA channel copies 16 KB of SRAM. The delay
between starting the DMA and the timed read was swept over 0-127 cycles; the figures
are min-max over those phases, and on the RP2350 all phases gave the same result. The
DMA kept running through every sample (~0.95 words per cycle in the same group). Both
RP2350 boards gave identical cycle counts (results_15), and two RP2040 boards gave
35-36 % (results_14 and 15). The earlier RP2040 "+24 %" came from one fixed phase that
depended on the code layout: it read 24 % for several builds, then 36 %. On the RP2350
this test reported N/A until 2026-11-07, because it looked for free SRAM in the bank
group the firmware fills.

The RP2350's bus fabric shares the banks far better than the RP2040's: a DMA copy in the
same bank group costs the CPU 4 % there, against 36 % on the RP2040.

## Flash and XIP

### Read modes and flash clock

Uncached 4 KB sequential read at clk_sys = 150 MHz, in MB/s. Both RP2350 boards give the
same cycle counts, to the cycle, in every mode they both support. "Continuous" = the chip
is put in continuous read mode, so no command byte is sent per access (as the RP2040's
boot stage 2 does). Every setting was verified by reading 16 x 128 KB against checksums
taken in the boot mode: **no errors in any combination**.

| Read mode | div 4 (SCK 37.5 MHz) | div 3 (SCK 50 MHz) | div 2 (SCK 75 MHz) |
|---|---|---|---|
| 03h serial (1-1-1) | 3.3 | 4.3 | 6.2 |
| 0Bh fast serial (1-1-1) | 3.1 | 4.0 | 5.8 |
| BBh dual (1-2-2), command each access | 5.5 | **7.2** (RP2350A boot mode) | 10.2 |
| BBh dual, continuous read | - | 8.1 | 11.4 |
| EBh quad (1-4-4), command each access | 8.5 | **11.0** (RP2350B boot mode) | 15.1 |
| EBh quad, continuous read | - | 13.1 | **17.7** |

The RP2040 reads 9.6 MB/s in its boot mode (quad, continuous read, SCK 62.5 MHz).

**Quad on the RP2350A** needs the flash's Quad Enable bit, which is clear on this board.
The chip accepts a **volatile** QE write (50h + 31h), which is lost at power-off. With QE
set, the RP2350A reads quad at exactly the WeAct's figures. Without it, quad reads return
wrong data, and the boot ROM therefore picks dual. The dual-mode penalty of this board is
purely that one bit.

### Cost of one flash cache miss

A cache miss fetches one 8-byte line in one SPI frame. On the RP2350 its cost is **(SCK
cycles of the frame) x clock divider + ~5 cycles**. The frame is command + address + mode
byte + dummy + 64 data bits, each at its bus width. This model predicts every measured
RP2350 figure to within 6 cycles.

The RP2040 hands the word to the CPU once the first 32 data bits are in: **20 SCK cycles
x divider + ~13-15 cycles**. That gives 53 cycles at divider 2 and 95 at divider 4, the
latter measured in the overclocking run.

| Setting | Frame (SCK cycles) | Model | Measured | Time |
|---|---|---|---|---|
| RP2040 boot: quad, continuous, div 2 (62.5 MHz) | 0+6+2+4+8 = 20 to the first word | 40 + ~13 | 53 cyc @125 MHz | **0.42 us** |
| RP2350A boot: dual + command, div 3 | 8+12+4+0+32 = 56 | 168 | 173 cyc | 1.15 us |
| dual, continuous, div 3 | 48 | 144 | 149 cyc | 0.99 us |
| dual, continuous, div 2 | 48 | 96 | 102 cyc | 0.68 us |
| quad + command, div 4 | 8+6+2+4+16 = 36 | 144 | 149 cyc | 0.99 us |
| RP2350B boot: quad + command, div 3 | 36 | 108 | 113 cyc | 0.75 us |
| quad + command, div 2 | 36 | 72 | 78 cyc | 0.52 us |
| quad, continuous, div 3 | 28 | 84 | 89 cyc | 0.59 us |
| **quad, continuous, div 2** | 28 | 56 | **62 cyc** | **0.41 us** |

All RP2350 rows were measured on both boards with identical results. The RP2040 has the
fastest miss of the three boards as they boot. With quad continuous read at SCK 75 MHz,
both RP2350 boards match it.

### Fastest flash clock per operating point

PicoBench test 433 tries quad continuous read at every clock divider and every RX sample
delay. Each setting is checked with 4 x 128 KB reads; the best one gets 16 x 128 KB more
and is measured. Divider 1 means SCK = clk_sys. The normal firmware stops at SCK 104 MHz;
120 and 133 MHz come from the overclocking firmware, which allows up to 133 MHz. SCK above
133 MHz was never tried. Results are identical on both RP2350 boards (Boya and XMC flash)
except where noted.

| clk_sys | Divider (SCK) | Working RX delays | Miss, M33 / Hazard3 | Uncached 4 KB |
|---|---|---|---|---|
| 48 MHz | 1 (48 MHz) | 0-1 of 0-7 | 33 / 31 cyc = 688 / 646 ns | 9.4 MB/s |
| 96 MHz | 1 (96 MHz) | 1-2 | 34 / 32 cyc = 354 / 333 ns | 18.0 MB/s |
| 120 MHz | 1 (120 MHz) | 1-2 | 34 / 32 cyc = 283 / 267 ns | 22.5 MB/s |
| **133 MHz** | **1 (133 MHz)** | 1-2 | **34 / 32 cyc = 256 / 241 ns** | **24.9 MB/s** |
| 150 MHz | 2 (75 MHz) | 0-3 (RP2350A: 1-3) | 61-62 / 59-60 cyc = 407-413 / 393-400 ns | 17.7-18.3 MB/s |

The frame model gives 28 x 1 + 5 = 33 cycles at divider 1, and the measurements are
within one cycle of it. Hazard3 needs 2 cycles fewer than the M33 in every mode. **The
fastest flash miss on any board is 133 MHz with divider 1**: 0.26 us, against 0.41 us at
150 MHz. The flash chips' own rated maximum clocks were not checked against datasheets.

The RP2040 runs SSI divider 2 at every operating point (SCK up to 100 MHz at 200 MHz).
Its working RX sample delays are 0-1 up to 133 MHz, **only 1 at 150 MHz** (SCK 75 MHz),
and 1-2 at 200 MHz (SCK 100 MHz). The 150 MHz point has no RX-delay margin left: one
clk_sys cycle is 6.7 ns, close to the width of the data window.

### Other flash figures

| | RP2040 | RP2350A | RP2350B |
|---|---|---|---|
| Cache hit | 10 cyc | 4 cyc | 4 cyc |
| Cache: 16 KB, 8-byte lines, shared with PSRAM | yes | yes | yes |
| Cached streaming, 64 KB (larger than the cache), M33 / H3 | 9.3 MB/s | 7.4 / 11.7 MB/s | 11.3 / 21.8 MB/s |
| Same loop from flash with a cold cache, overhead vs SRAM | +19 % | +57 % / +45 % | +32 % / +25 % |
| Program one 256-byte page | 372 us | 869 us | 580 us |
| Erase one 4 KB sector (average of 4) | 7-25 ms (varies between runs)\* | 6.8 ms | 3.9 ms |

\* The RP2040's Boya flash alternates between ~0.3 ms and ~28 ms for a 4 KB erase from
run to run, depending on the state the previous run left the sector in. Probably it
skips erasing blank sectors.

The "cold code" overhead depends on where the linker places the loop and moves by up to
~10 points between builds. Hazard3 streams cached flash about twice as fast as the M33.

## PSRAM (WeAct RP2350B only)

AP Memory APS6404, 8 MB, with the SDK's default timing profile. All 8 MB passed the
integrity tests, and all 9 tested divisor / RX-delay combinations were stable.

| at 150 MHz (PSRAM SCK 75 MHz) | M33 | Hazard3 |
|---|---|---|
| Uncached single read | 55 cyc (367 ns) | 59 cyc\* |
| Cache miss (8-byte line) | 66 cyc (440 ns) | 64 cyc |
| Cache hit / dependent load through the cache (8 KB) | 4 / 2.25 cyc | 8\* / 3.05 cyc |
| Dependent load, random over 256 KB: cached / uncached | 66.9 / **53.1** cyc | not measured |
| Uncached stream read / write | 27.3 / 33.7 MB/s | 27.5 / 34.2 MB/s |
| DMA: PSRAM to SRAM / SRAM to PSRAM / PSRAM to PSRAM | 29.1 / 34.7 / 8.0 MB/s | same |

\* Measured with the sampling loop in flash. Its instruction fetches share the XIP port
with the PSRAM access and added 4 cycles: with the loop in SRAM the M33 figures fell from
59 to 55 and from 8 to 4. The Hazard3 run was not repeated.

**Random access to a large working set is faster uncached.** The dependent-load rows walk
a random cycle through every word of the working set once per sample (results_15). 8 KB
fits in the 16 KB XIP cache, so every load hits. At 256 KB nearly every load misses, and
a cached miss fetches a whole 8-byte line (66 cycles) where an uncached read fetches one
word (55). Through the cached alias that costs 66.9 cycles per load, through the
uncached alias 53.1, 21 % less. The ratio is the same at every clock (36.0 / 29.1 cycles
at 48 MHz with divider 1). For tables much larger than the cache that are read at random,
use the uncached alias. The earlier figure of ~3.3 cycles for "256 KB cached" walked only
256 steps (2 KB), which fit in the cache.

**The SDK picks the PSRAM divider by clock.** It uses divider 1 at 48 and 96 MHz (PSRAM
SCK = clk_sys) and divider 2 from 120 MHz up. A single uncached read therefore costs 32 cycles
(333 ns) at 96 MHz but 55 cycles (458 ns) at 120 MHz, and streaming is faster at 96 MHz
(28.1 MB/s) than at 120 MHz (21.8 MB/s).

**Divider 1 works up to 133 MHz**, and even at 150 MHz. That is inside the APS6404's
133 MHz rating but outside the SDK's own rule, which never uses divider 1 above 100 MHz.
PicoBench test 513 checked every setting with a 256 KB write/read pattern:

| clk_sys / divider | PSRAM SCK | Working RX delays | Uncached read | 4 KB uncached |
|---|---|---|---|---|
| 150 MHz / 2 (SDK default) | 75 MHz | 1-4 | 55 cyc = 367 ns | 27.3 MB/s |
| 48 MHz / 1 | 48 MHz | 0-1 | 30 cyc = 625 ns | 14.9 MB/s |
| 96 MHz / 1 | 96 MHz | 1-2 | 32 cyc = 333 ns | 28.1 MB/s |
| 120 MHz / 1 | 120 MHz | 1-2 | 33 cyc = 275 ns | 35.3 MB/s |
| **133 MHz / 1** | 133 MHz | **2 only** | **33 cyc = 248 ns** | **39.1 MB/s** |
| 150 MHz / 1 (beyond the chip's rating) | 150 MHz | 2-3 | 36 cyc = 240 ns\* | 44.3 MB/s |

\* Measured with the older loop in flash, so about 4 cycles too high (about 32 cycles =
213 ns with the SRAM loop).

The fastest in-spec setting is 133 MHz with divider 1: 32 % less latency and 43 % more
throughput than the SDK's 150 MHz setting, despite the slower CPU. Its RX-delay window is
a single value, though, so it has no margin.

### Flash and PSRAM share one controller

A flash cache miss was measured while a PSRAM DMA transfer runs. The delay between DMA
start and the flash read was swept over 256 phases (0-2040 cycles), and the code ran
from SRAM:

| PSRAM traffic | flash miss p50 | flash miss worst | vs quiet |
|---|---|---|---|
| none | 113 cyc | 113 cyc (0.75 us) | - |
| DMA read stream (PSRAM to SRAM) | 129 cyc | 182 cyc (1.21 us) | +61 % |
| DMA write stream (SRAM to PSRAM) | 129 cyc | 151 cyc (1.01 us) | +33 % |
| DMA copy (PSRAM to PSRAM) | 207 cyc | **277 cyc (1.85 us)** | **+145 %** |

The M33 and Hazard3 agree to within 4 cycles. A long PSRAM read burst does not hold the
controller for its whole max-select window: the flash request waits at most ~70 cycles.
Streaming collapses in both directions:
- Cached flash streaming slows ~30x while a PSRAM copy runs (2446 to 77 422 cycles per
  4 KB).
- Uncached PSRAM streaming slows **6.1x** while a DMA reads flash through the uncached
  alias (22 479 to 138 125 cycles per 4 KB).

The earlier "PSRAM loses only ~1 %" was wrong. That DMA read the *cached* flash alias, so
most of its reads were cache hits that never reached the flash controller, and the result
moved with the code layout (2 % in one build, 20 % in the next).

## Cartridge hot path

One emulated console bus cycle: read the address bus (SIO), decode bank and offset,
fetch the byte from a 4 KB table, and write the data port (SIO). 200 000 iterations with
interrupts off, unless stated otherwise. Values are p50 / p99.9 / max in cycles.

| Variant | RP2040 @125 MHz | RP2350A M33 / H3 @150 MHz | RP2350B M33 / H3 @150 MHz |
|---|---|---|---|
| table in SRAM, code in SRAM | 33 / 40 / 40 | 20 / 23 / 23 and 18 / 19 / 19 | 20 / 23 / 23 and 18 / 19 / 19 |
| table in SRAM, code in flash (one cold code fetch) | 33 / 40 / 743 | 20 / 23 / 961 | 20 / 23 / 553 |
| table in flash, warm cache | 33 / 84 / 90 | 20 / 200 / 201 | 20 / 140 / 141 |
| table in flash, cold cache | 84 | 200 / 198 | 140 / 138 |
| table in flash, uncached | 82 | 116 / 114 | 98 / 96 |
| table in PSRAM, cached | - | - | 20 / 87 / 88 |
| table in PSRAM, uncached | - | - | 59 / 57 |
| SRAM table, 10 kHz timer interrupt | 33 / 328 / 329-335 | 20 / 266 / 266-2414\* | 20 / 231-266 / 234-1556\* |
| SRAM table, DMA copying in SRAM | 34 / 41 / 41 | 20 / 23 / 23 | 20 / 23 / 23 |
| SRAM table, DMA streaming PSRAM | - | - | 20 / 23 / 23 |
| flash table, DMA streaming PSRAM | - | - | 22 / 71 / 171-203\*\* |
| other core running the ARM interpreter (max) | 45 (vs 40) | 25 (vs 23) | 25 (vs 23) |

\* The maximum is a single sample: the interrupt handler's first run, fetched cold from
flash. It changes with the code layout between builds (M33 builds from results_8 to
results_15); the p99.9 does not.

\*\* The main run (build 425991b6) read 21 / 150 / 199. Every later build, from 80ea42b1
on, gives a p99.9 of 70-71 cycles.

The access pattern (sequential, random, bank-switching) makes no difference on any board.
DMA in SRAM or on the PSRAM does not disturb an SRAM-resident hot path. Anything that
reads flash or uncached PSRAM costs a full miss (previous section) in the worst case.

## Scaling with the system clock

The flash clock divider is fixed (RP2040: 2, RP2350: 3), so the flash SCK scales with
clk_sys. On the RP2040 at 200 MHz that means SCK 100 MHz, which PicoBench verifies before
keeping it. Every latency in this document is therefore **the same number of cycles at
every operating point**, and its time in nanoseconds scales as 1/f. The one exception is
PSRAM, whose divider the SDK changes at 120 MHz (see above).

Hot path p50 in nanoseconds (cycles in brackets):

| clk_sys | RP2040 SRAM (33) | RP2040 flash cold (84) | RP2350 SRAM, M33 (20) | RP2350A flash cold (200) | RP2350B flash cold (140) |
|---|---|---|---|---|---|
| 48 MHz | 688 | 1750 | 417 | 4167 | 2917 |
| 96 MHz | 344 | 875 | 208 | 2083 | 1458 |
| 120 MHz | 275 | 700 | 167 | 1667 | 1167 |
| 125 MHz | 264 | 672 | - | - | - |
| 133 MHz | 248 | 632 | 150 | 1504 | 1053 |
| 150 MHz | 220 | 560 | **133** | **1333** | **933** |
| 200 MHz | **165** | **420** | - | - | - |

Throughput scales the same way. For example, uncached flash reads go from 3.7 MB/s at
48 MHz to 15.3 MB/s at 200 MHz on the RP2040, from 2.3 to 7.2 MB/s on the RP2350A and
from 3.5 to 11.0 MB/s on the RP2350B. DMA moves ~3.98 bytes per cycle on all boards
(191 MB/s at 48 MHz, 597 MB/s at 150 MHz).

## Peripherals and interrupts

| | RP2040 | RP2350 M33 | RP2350 Hazard3 |
|---|---|---|---|
| DMA copy 16 KB (bytes per cycle) | 3.97 | 3.98 | 3.98 |
| DMA single-word transfer | 29 cyc | 16 cyc | 15 cyc |
| DMA, 4 channels at once (aggregate) | - | 399 MB/s | 399 MB/s |
| PIO: CPU to PIO to CPU round trip | 20 cyc | 17 cyc | 14 cyc |
| PIO fed by DMA | 1.79 cyc/word (280 MB/s) | 1.55 cyc/word (386-391 MB/s) | 1.36 cyc/word (437-442 MB/s) |
| SIO GPIO register write / read | 13 cyc | 8 cyc | 9 cyc |
| GPIO output-to-input turnaround (internal) | 20 cyc | 17 cyc (RP2350B) | 14 cyc (RP2350B) |
| Software interrupt entry p50 | 25 cyc | 25 cyc | 45 cyc |
| Software interrupt round trip p50 | 60 cyc | 48 cyc | 94 cyc |
| Core 1 launch | 46 us | 44 us (RP2350B), 76 us (RP2350A) | 58 us (RP2350B) |
| Inter-core FIFO round trip p50 | 88 cyc | 68-75 cyc | 66-71 cyc |
| Spinlock acquire + release p50 | 17 cyc | 14 cyc | 16 cyc |

The worst cases of interrupt and core-launch figures contain cold code fetches from
flash, so they are 1.6-2x larger on the RP2350A (dual mode) than on the RP2350B. For
example, interrupt entry max is 1044-1310 vs 624-818 cycles on the M33, depending on where
the build puts the handler. The medians are identical. The turnaround test found
no free pin on the RP2350A board (GPIO 20-22 and 26-28 are held by something on the
board).

## ARM interpreter (Harmony cartridge code)

A Thumb (ARMv4T) interpreter running real ARM code of seven Atari 2600 Harmony cartridge
games. The code is replayed from recordings and checked bit for bit against the
Gopher2600 emulator. The game data is not distributed with PicoBench; see
[workload/README.md](../workload/README.md).

| | RP2040 (M0+) | RP2350 M33 | RP2350 Hazard3 |
|---|---|---|---|
| Cycles per guest instruction, all games | 42.3 | 28.2 | 24.4-24.5 |
| Guest MIPS at the default clock | 3.0 | 5.3 | 6.1 |
| Guest MIPS at the highest rated clock | 4.7 (200 MHz) | 5.3 (150 MHz) | 6.1 (150 MHz) |
| Guest MIPS at the highest overclock confirmed | 9.9 (420 MHz) | 14.6 (RP2350B, 412 MHz); 11.3 (RP2350A, 320 MHz) | 17.2 (RP2350B, 420 MHz) |
| Games whose need is met there (of 7) | 2 (Boom!, Wizard of Wor) | 2 | 4 (also Draconian, Galagon) |

The cycles per instruction are identical on both RP2350 boards and at every clock, and
the overclocked figures use them. The overclocking run measured the synthetic load
directly: 9.76 / 14.40 / 15.70 MIPS. The games need 8.9 (Boom!), 9.0 (Wizard of Wor),
16.3 (Draconian), 16.7 (Galagon), 20.5 (MK), 26.8 (Stratovox) and 29.5 (Spiders) guest
MIPS. Most of the games need 16-30 guest MIPS. That is 2-3x more than a decode-on-the-fly
interpreter delivers, so a faster executor, not a faster clock, is what would close the
gap.

## Overclocking (outside specification)

The overclocking firmware (`PICOBENCH_VARIANT=overclock`, `dist/*_overclock.uf2`) raises
clk_sys in 10 MHz steps at each core voltage from 1.10 to 1.30 V. 1.30 V is the
regulator's limit while it is locked. Each step lasts 250 ms:
- both cores run a checked integer kernel;
- core 0 also runs the ARM interpreter on a synthetic load, SRAM and DMA patterns, a
  flash read-back and a PSRAM pattern.

During the search the flash stays at SCK 75 MHz or less and PSRAM at 133 MHz or less, so
only the microcontroller runs outside its rating (the flash probe below goes up to
133 MHz). The last good step is then run again for 1.5 s.
A hang is caught by the watchdog, and the run goes on with the next voltage. Four runs
(results_10 to results_13) are combined below. Values are the highest step passed in
MHz. ✓ = confirmed for 1.5 s, H = the next step hung, so the value is not confirmed.

| Core voltage | 1.10 V | 1.15 V | 1.20 V | 1.25 V | 1.30 V |
|---|---|---|---|---|---|
| RP2040 (M0+) | 309-320 ✓ | 351-360 H | 380 H | 412 ✓, 420 H | 420 ✓, 432-450 H |
| RP2350B, M33 | 309 H | 330 ✓ | 360 ✓ | 390 ✓ | 412 ✓ (420: one pass, one fail) |
| RP2350B, Hazard3 | 309 H | 340 ✓ | 369 ✓ | 400 ✓ | 420 ✓ (432 fails) |
| RP2350A, M33 | 320 H | 320 ✓ | 320 H | 320 H | hangs at 320 |
| RP2350A, Hazard3 | 320 H | hangs at 320 | hangs at 320 | hangs at 320 | hangs at 320 |

- **RP2040 and WeAct RP2350B scale with voltage.** At 1.30 V they reach 420-450 MHz
  (2.1-2.2x the RP2040's 200 MHz rating) and 412-420 MHz (2.7-2.8x the RP2350's
  150 MHz). Hazard3 runs ~10 MHz higher than the M33
  on the same chip. Chip temperature stayed at 31-41 °C.
- **The first check to fail** is almost always the ARM interpreter, or core 1 not
  starting. Only once did the integer kernel itself go wrong.
- **The RP2350A clone stops at ~320 MHz at every voltage.** More voltage makes it worse:
  at 1.15 V and up, Hazard3 hangs even at 320 MHz. At 1.10 V it matches the WeAct, so
  the chip is not the limit. The most likely cause is this board's supply for the
  on-chip switching regulator (external inductor and capacitors), but that is not
  proven. Its temperature sensor reads implausible values, so the explorer's 80 °C stop
  cannot protect it.
- **At 390 MHz with PSRAM divider 3** (SCK 130 MHz), an uncached PSRAM read takes 207 ns.
  The explorer's main checks keep the flash in its boot mode at SCK 65-75 MHz, where a
  miss costs ~520-570 ns. The flash probe below measures the fastest setting.

**Flash at the overclocked maximum** (results_12 and 13). The explorer returns once more
to a clock that has already passed and tries quad continuous read with SCK at most
133 MHz, scanning every RX delay. That clock is the confirmed maximum, or the start of a
stage, which already passed at a lower voltage:

| clk_sys | Divider (SCK) | Working RX delays | Miss | Uncached 4 KB |
|---|---|---|---|---|
| 330 MHz (M33) | 3 (110 MHz) | 1-6 | 90 cyc = 272 ns | 28.2 MB/s |
| 360 MHz (M33) | 3 (120 MHz) | 2-6 | 90 cyc = 250 ns | 30.8 MB/s |
| **390 MHz (M33)** | **3 (130 MHz)** | 2-6 | **90 cyc = 230 ns** | **33.4 MB/s** |
| 412 MHz (M33) | 4 (103 MHz) | 1-7 | 118 cyc = 286 ns | 27.8 MB/s |
| 420 MHz (Hazard3) | 4 (105 MHz) | 1-7 | 116 cyc = 276 ns | 28.5 MB/s |
| RP2040, 309 MHz | SSI 4 (77 MHz) | 1-3 of 0-7 | 95 cyc = 307 ns | - |
| RP2040, 351 MHz | SSI 4 (88 MHz) | 1-4 | 95 cyc = 270 ns | - |
| RP2040, 380 MHz | SSI 4 (95 MHz) | 1-4 | 95 cyc = 250 ns | - |
| **RP2040, 412 MHz** | **SSI 4 (103 MHz)** | 1-4 | **95 cyc = 230 ns** | - |

- **The frame models hold.** RP2350: 28 x 3 + 5 = 89 cycles and 28 x 4 + 5 = 117.
  RP2040: 20 x 4 + ~15 = 95.
- **The best miss is 0.23 us on both chips**: the RP2350 at 390 MHz with divider 3, the
  RP2040 at 412 MHz with SSI divider 4. On the RP2350 that is only 12 % better than the
  in-spec best (0.26 us at 133 MHz with divider 1). The flash SCK sets the miss
  time, not the CPU clock, and above 400 MHz the divider has to go up to 4.
- **The RX-delay windows are wide** because RXDELAY counts half cycles of a much faster
  clock.
- **On the RP2040 every stage ended in a hang** before its confirmation. Its flash
  figures come from the probe at each stage's start.

These are limits of one chip of each kind, at room temperature, over seconds - not
clocks to run at. Leave a margin.

## Conclusions

1. **The two RP2350 boards are the same chip in cycles.** All differences come from the
   flash read mode the boot ROM picks: dual on the black clone, whose QE bit is clear,
   and quad on the WeAct. With QE set (volatile is enough), the black clone reads quad
   exactly like the WeAct.
2. **A flash cache miss costs one SPI frame** (frame SCK cycles x divider + ~5). The
   best setting tested on the RP2350 is quad with continuous read at SCK 75 MHz:
   **0.41 us** per miss and 17.7 MB/s on both boards. That matches the RP2040's 0.42 us.
   The boot defaults are 1.15 us (black clone) and 0.75 us (WeAct).
3. **For hard real-time loops, SRAM decides.** An SRAM-resident hot path takes 120-133 ns
   on the RP2350 and 264 ns on the RP2040 at their default clocks. It is unaffected by
   flash mode and by DMA in SRAM or PSRAM. A 10 kHz interrupt adds ~250 cycles to its
   p99.9. Bulk SRAM reads do feel a DMA copy in the same banks: +36 % on the RP2040,
   +4 % on the RP2350, and nothing when the RP2350's DMA works in the other bank group.
4. **Hazard3 vs M33:** Hazard3 is faster for ALU work, copies, the hot path (18 vs 20
   cycles) and interpretation (24.4 vs 28.2 cycles per instruction). The M33 is faster
   for dependent loads and division.
5. **PSRAM** is as fast as SRAM when cached and ~0.37 us per uncached access. For random
   reads over a working set much larger than the 16 KB cache, the uncached alias is 21 %
   faster than the cached one (53 vs 67 cycles per load). PSRAM DMA
   can stretch a flash miss to 1.85 us and slow cached flash streaming ~30x. A DMA
   reading flash slows uncached PSRAM streaming 6x. Keep time-critical code and data out
   of flash and PSRAM while the other one is busy.
6. **Clock:** every latency in cycles is constant from 48 to 200 MHz, so time scales
   1/f. The exception is PSRAM, which is faster per access at 96 MHz than at 120-133 MHz
   because of the SDK's divider choice.
7. **For memory, 133 MHz beats 150 MHz.** On the RP2350, divider 1 at 133 MHz gives the
   fastest flash miss (0.26 us) and the fastest in-spec PSRAM read (0.25 us). The CPU is
   11 % slower than at 150 MHz. The PSRAM is rated for 133 MHz. The flash read correctly
   at SCK 133 MHz on both boards, but its rated maximum was not checked. The RP2040 at 200 MHz
   (1.15 V) runs everything 1.6x faster than at 125 MHz, with flash SCK 100 MHz.
8. **Overclocking** roughly doubles to triples the CPU clock on the RP2040 and on the
   WeAct RP2350B. It still does not bring the ARM interpreter to the speed most of the
   games need: at best 4 of 7, on Hazard3 at 420 MHz. A flash miss gains little from
   it, 0.23 us at best, because the flash clock stays at 133 MHz or less.

## Caveats

- One board of each kind (two RP2040 boards for the last runs), at room temperature. Each flash setting was verified on 2 MB
  of reads, each PSRAM setting on 256 KB patterns. That is not a long-term stability
  proof, least of all for the settings with a single working RX delay. The flash chips'
  rated maximum clocks were not checked against datasheets.
- Quad, continuous-read and divider-1 settings on the RP2350 were tried by PicoBench tests
  431-433 and restored afterwards. They are not what the boards use by default.
- Until 2026-11-07 the flash and PSRAM sampling loops ran from flash. Their instruction
  fetches shared the XIP port with the access being measured. Cache hits read 12 / 8 / 8
  cycles instead of 10 / 4 / 4, and single uncached reads 4 cycles too long. Older reports
  carry those figures.
- The RP2350A's internal temperature sensor reads implausible values (-8 to -13 °C),
  probably a different ADC reference on that board. It does not affect any timing.
- Numbers come from PicoBench's REPORT.TXT / RESULTS.CSV of the runs listed under
  "Data"; those files stay on the boards' USB disks and are not part of this repository.
