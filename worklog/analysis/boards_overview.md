# Boards tested so far

Data:
- RP2040: `/Volumes/Public/PicoBench/results_7/rp2040/RUN0001` (build of 2026-11-06, `_local`);
  apart from the cold miss it repeats results_5.
- RP2350A and RP2350B: `/Volumes/Public/PicoBench/results_6` (rp2350a/arm,
  rp2350a/riscv/RUN0002, rp2350b/arm, rp2350b/riscv), `_local` images of the 2026-10-02
  build, 107 stages including `flash.quad_enable` (431). Every figure that both runs have
  matches results_5 to the cycle unless the table says otherwise.
- The three cold-miss figures (`flash.cache`, `psram.bench`, `qmi.conflict`) come from
  `/Volumes/Public/PicoBench/results_7` (build of 2026-11-06, rp2350a/arm, rp2350b/arm,
  rp2350b/riscv): their loops now run from SRAM, the earlier values were inflated by
  instruction fetches from flash. Everything else in results_7 repeats results_6.
- The WeAct was also run without PSRAM (`results_1`, older firmware).

Timings at the default clock: 125 MHz (RP2040), 150 MHz (RP2350). "M33 / H3" = Cortex-M33 /
Hazard3 on the same board. Full tables: `results5_comparison.md`.

| | **RP2040** (purple clone) | **RP2350A** (black clone, Pico 2 type) | **RP2350B** (WeAct Core + PSRAM) |
|---|---|---|---|
| **Chip** | | | |
| Silicon | RP2040-B2 | RP2350-A2 (errata E9, E14) | RP2350-A3 |
| Package / GPIO | QFN56 / 30 | QFN60 / 30 | QFN80 / 48 |
| Boot ROM version | 3 | 2 | 4 |
| Cores tested | 2x Cortex-M0+ | 2x M33, 2x Hazard3 | 2x M33, 2x Hazard3 |
| SRAM / DMA / PIO | 264 KB / 12 ch / 2 | 520 KB / 16 ch / 3 | 520 KB / 16 ch / 3 |
| Clocks tested (default) | 48-133 MHz (125) | 48-150 MHz (150) | 48-150 MHz (150) |
| XOSC / ROSC (uncalibrated) | 12.000 MHz / 5.4 MHz | 12.000 MHz / 11.8 MHz | 12.000 MHz / 45.4 MHz |
| VREG | 1.10 V | 1.10 V | 1.10 V |
| Temperature at run | 25.2 °C | -13.1 / -8.4 °C (implausible, see notes) | 27.1 / 28.5 °C |
| **Board** | | | |
| LED | GPIO25 | GPIO25 | GPIO25 |
| Buttons | BOOTSEL | BOOTSEL | BOOTSEL + KEY (GPIO23) |
| Pico-style VSYS divider on GPIO29 | no | no | n/a |
| Free GPIO | 29 (0-24, 26-29) | 29 (0-24, 26-29) | 45 (1-22, 24, 26-47) |
| GPIO turnaround test pin | GPIO28 | none free (28/27/26/22/21/20 held externally) | GPIO28 |
| **Flash** | | | |
| Vendor (JEDEC) | **Boya** (68 40 18) | **Boya** (68 40 18), older revision | **XMC** (20 40 18) |
| Size | 16 MB | 16 MB | 16 MB |
| SFDP | rev 1.6, 16-dword BFPT | rev 1.0, 9-dword BFPT | rev 1.6, 16-dword BFPT |
| Quad Enable (SR2 bit 1) at boot | set (by boot stage 2) | **not set** | set |
| QE can be set? (test 431) | n/a (fixed by stage 2) | **yes**: volatile 50h + 31h accepted, cleared again after the test | already set, nothing written |
| Controller | SSI + boot stage 2 | QMI | QMI |
| Read mode at boot (cmd) | quad (0xEB) | **dual 1-2-2 (0xBB)** | **quad 1-4-4 (0xEB)** |
| SCK | 62.5 MHz (div 2) | 50 MHz (div 3) | 50 MHz (div 3) |
| Uncached read, 32-bit, boot mode | 9.6 MB/s | 7.2 MB/s | 11.0 MB/s |
| Quad read with QE set, div 3 / div 2 (test 431) | - | **11.0 / 15.1 MB/s**, 0 errors | 11.0 / 15.1 MB/s, 0 errors |
| Cold miss, test 431 method: boot mode / quad div 3 / quad div 2 | - | 173 / **113** / 78 cyc | 113 / 113 / 78 cyc |
| Continuous read (no command byte, test 432): quad div 3 / quad div 2 | (boot default) | **89 / 62 cyc**, 13.1 / 17.7 MB/s, 0 errors | 89 / 62 cyc, 13.1 / 17.7 MB/s, 0 errors |
| Continuous read, dual div 3 / dual div 2 (test 432, no QE needed) | - | 149 / 102 cyc, 8.1 / 11.4 MB/s, 0 errors | 149 / 102 cyc, 8.1 / 11.4 MB/s, 0 errors |
| Cache hit M33 / H3 | 12 cyc (96 ns) | 7 / 8 cyc (47 / 53 ns) | 7 / 8 cyc (47 / 53 ns) |
| Cache miss, cold (`flash.cache`, boot mode) M33 / H3 | **53 cyc (0.42 us)** | 173 cyc (1.15 us) | 113 / 111 cyc (0.75 / 0.74 us) |
| SPI frame per miss (SCK cycles x divider) | 28 x 2 (no command byte: continuous read) | 56 x 3 (command + dual) | 36 x 3 (command + quad) |
| Cached streaming M33 / H3 | 9.3 MB/s | 7.4 / 11.7 MB/s | 11.3 / 21.8 MB/s |
| Code from flash, cold (overhead vs SRAM) M33 / H3 | +19 % | +49 / +45 % | +29 / +25 % |
| Program 256 B page M33 / H3 | 368 us | 868 / 909 us | 579 / 646 us |
| Erase 16 KB (4 sectors) | 29.9 ms | 27.2 ms | 15.7 ms |
| **PSRAM** | | | |
| Fitted | no (RP2040 has no QMI) | no (nothing on GPIO 0/8/19) | **AP Memory APS6404, 8 MB, CS = GPIO0** |
| Integrity / timing combinations | - | - | 0 errors over 8 MB / 9 of 9 stable |
| Uncached single read M33 / H3 | - | - | 58 / 59 cyc |
| Cache miss M33 / H3 | - | - | 66 / 64 cyc |
| Uncached stream read / write | - | - | 27 / 34 MB/s |
| Flash cold miss under PSRAM DMA, worst over 256 phases M33 / H3 | - | - | quiet 113 / 111; read stream 182 / 178; write stream 151 / 149; copy **277 / 274 cyc (1.85 us, +145 %)** |
| **Performance** | | | |
| Hot path, SRAM (p50 / max) M33 / H3 | 33 / 40 cyc = 264 / 320 ns | 20 / 23 and 18 / 19 cyc = 133 / 120 ns | 20 / 23 and 18 / 19 cyc = 133 / 120 ns |
| Hot path, table in flash, cold cache M33 / H3 | 84 cyc (672 ns) | 200 / 198 cyc (1.33 us) | 140 / 138 cyc (0.93 us) |
| Hot path, table in PSRAM, uncached M33 / H3 | - | - | 59 / 57 cyc (393 / 380 ns) |
| SRAM dependent load M33 / H3 | 3.52 cyc | 2.26 / 3.02 cyc | 2.26 / 3.02 cyc |
| DMA copy 16 KB | 496 MB/s | 597 MB/s | 597 MB/s |
| Software IRQ entry (p50 / max) M33 / H3 | 25 / 428 cyc | 25 / 1044 and 45 / 698 cyc | 25 / 624 and 45 / 434 cyc |
| ARM interpreter, cycles per guest instr. M33 / H3 | 42.3 | 28.2 / 24.5 | 28.2 / 24.5 |
| ARM interpreter projected to 250 MHz M33 / H3 | 5.9 MIPS | 8.8 / 10.2 MIPS | 8.8 / 10.2 MIPS |
| Games fast enough at 250 MHz (of 7) M33 / H3 | 0 | 0 / 2 | 0 / 2 |
| **Result** | results_5: PASS, 0 errors | results_6: PASS, 0 errors (both cores) | results_6: PASS, 0 errors (both cores) |

## Quad with QE set (`flash.quad_enable`, 431, results_6)

Same method on both RP2350 boards (16 x 128 KB verified per setting, M33 figures; Hazard3
within 2 cycles). The RP2350A's QE was set in the volatile status register and cleared
again afterwards.

| at 150 MHz | RP2350A default (dual, div 3) | RP2350A quad div 3 | RP2350B quad div 3 | quad div 2 (SCK 75 MHz), both boards |
|---|---|---|---|---|
| 4 KB uncached read | 85 232 cyc, 7.2 MB/s | 56 048 cyc, 11.0 MB/s | 56 048 cyc, 11.0 MB/s | 40 817 cyc, 15.1 MB/s |
| cold cache miss (this test's method) | 173 cyc (1.15 us) | 113 cyc (0.75 us) | 113 cyc (0.75 us) | 78 cyc (0.52 us) |
| errors | 0 | 0 | 0 | 0 |

With QE set, the RP2350A's Boya reads quad **cycle-identical** to the WeAct's XMC at every
divider: the dual-mode penalty was purely the QE bit. The miss figures here are lower
than `flash.cache` (411) reports (173 vs 283 dual, 113 vs 175 quad) because the methods
differ; the quad/dual ratio agrees (0.65 vs 0.62).

## Notes

- Both clones carry a Boya BY25Q128-class chip (68 40 18), but not the same revision: the
  RP2040 board's answers SFDP 1.6 with a 16-dword parameter table, the RP2350A board's SFDP
  1.0 with 9 dwords, and it programs pages 2.4x slower. On the RP2040 the boot stage 2
  sets QE and runs quad; on the RP2350A the boot ROM finds QE clear and falls back to dual.
  The chip accepts a volatile QE write (50h + 31h), and then reads quad exactly as fast as
  the WeAct (results_6, test 431).
- Cold misses are measured from SRAM since results_7 (`bench_cold_reads`): `flash.cache`
  now reads 53 / 173 / 113 cycles (RP2040 / RP2350A dual / RP2350B quad), the same as
  `flash.quad_enable`; before, with the loop in flash, it read 163 / 283 / 175.
- **A flash miss costs one SPI frame**: (SCK cycles per 8-byte line fill) x divider + ~5
  cycles on the RP2350 (-3 on the RP2040). Frame = command + address + mode + dummy + 64
  data bits at the respective widths: RP2350B quad 8+6+2+4+16 = 36, RP2350A dual
  8+12+4+0+32 = 56, RP2040 quad with continuous read 0+6+2+4+16 = 28. It predicts all
  five measured figures (53, 173, 113, and 149 / 78 for quad div 4 / div 2) within 5
  cycles, and `hotpath.memory` cold-cache minus SRAM agrees (51 / 180 / 120).
- **Why the RP2040 misses fastest** (0.42 us vs 0.75 us on the RP2350B): its boot stage 2
  puts the flash in continuous read mode, so no command byte is sent per miss (8 of 36
  SCK cycles on the RP2350), and it clocks SCK at 62.5 MHz (div 2) against 50 MHz. The
  RP2350 boot ROM sends 0xEB / 0xBB on every miss. The model predicted 89 / 61 cycles for
  quad continuous read at div 3 / div 2 and 149 / 101 for dual; `flash.continuous_read`
  (432, results_8) measured **89 / 62** and **149 / 102** on both RP2350 boards, cycle-identical.
- **Best RP2350 setting measured: quad + continuous read at div 2 (SCK 75 MHz) = 62 cycles
  = 0.41 us per miss, 17.7 MB/s uncached** - slightly faster than the RP2040 (0.42 us),
  2.8x faster than the RP2350A's boot default (1.15 us) and 1.8x faster than the RP2350B's
  (0.75 us). At SCK 50 MHz (div 3) it is 89 cycles (0.59 us). The black board needs QE set
  for it (volatile is enough); without QE, dual continuous read at div 2 gives 102 cycles.
  All four combinations read 16 x 128 KB without error on both boards (room temperature;
  the Boya's and XMC's rated maximum clocks not checked).
- RP2350A and RP2350B are cycle-identical in everything that does not read flash; the worst
  cases that include a cold flash miss (IRQ entry, first call into SDK code) are 1.6-2x
  larger on the dual-mode board. Everything in the "Performance" rows above that involves
  flash refers to the boot mode; with quad enabled the RP2350A would match the RP2350B.
- `qmi.conflict` before 2026-11-06 gave 170 -> 514, 171 -> 449 or 221 -> 291 cycles
  depending only on the build: its loop ran from flash, so instruction fetches fell inside
  the timed read and queued behind the PSRAM traffic as well. The fixed test (results_7)
  runs each sample from SRAM and sweeps the DMA-to-read phase: the flash miss under PSRAM
  DMA is p50 129 / max 182 (read stream), p50 129 / max 151 (write stream), p50 207 / max
  277 (copy) on M33, and within 4 cycles of that on Hazard3. The worst phases are spread
  over the sweep (96-1512 cycles), so the 0-2040 range covers them.
- The RP2350A temperature (-9.8, -13.1, -8.4 °C over three runs; raw 952-962 on ADC
  channel 4) is not believable. A lower ADC reference on that clone would explain it (with
  3.0 V instead of the assumed 3.3 V it reads ~28 °C) - not verified.
- The GPIO turnaround test found no free pin on the RP2350A board in every run (GPIO
  28/27/26/22/21/20 all held externally): something on that board is connected to those
  pins.
- With PSRAM fitted, GPIO0 must stay HIGH even when the firmware does not use the PSRAM
  (a low level selects the chip on the shared QSPI bus).
