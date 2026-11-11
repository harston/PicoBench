# results_5 - RP2040 purple vs RP2350A black (dual flash) vs WeAct RP2350B (quad + PSRAM)

Source: `/Volumes/Public/PicoBench/results_5`, all images from one `build.sh` run
(2026-09-29 13:26, firmware 0.0.1+9f87c89-dirty). Every run: OVERALL PASS, 0 crashes,
ERRORS.TXT "No errors". `rp2350a_local_riscv` held the plain RISC-V image (build
711c8a63, no workload) and was renamed `rp2350a_riscv`; there is no RP2350A Hazard3 run
with the workload.

Runs compared: rp2040_local, rp2350a_local_arm, rp2350a_riscv, rp2350b_local_arm,
rp2350b_local_riscv. Figures at 125 MHz (RP2040) / 150 MHz (RP2350) unless noted.

## Findings

**1. RP2350A (A2 silicon) and RP2350B (A3 silicon) are the same chip for everything that
does not touch flash.** CPU, SRAM, DMA, PIO, GPIO and the SRAM hot path give identical
cycle counts, test by test, on the same core (e.g. hot path 20/23/23 cycles p50/p99.9/max
on M33, 18/19/19 on Hazard3; SRAM dependent load 2.26 / 3.01 cycles; DMA 16 KB copy 4114
cycles).

**2. The only real difference is the flash read mode the boot ROM leaves: dual 1-2-2
(black, Boya 68 40 18, SFDP 1.0) vs quad 1-4-4 (WeAct, XMC 20 40 18).** Same QMI divider
(3, SCK 50 MHz) on both.

| at 150 MHz | RP2350A dual | RP2350B quad | B/A |
|---|---|---|---|
| uncached read 32-bit | 7.2 MB/s | 11.0 MB/s | 1.52x |
| cache miss, cold | 282 cyc (1.88 us) | 174 cyc (1.16 us) | 0.62x |
| hot path, table in flash, cold cache | 200 cyc | 140 cyc | 0.70x |
| hot path, table in flash, uncached | 116 cyc | 98 cyc | 0.84x |
| loop from flash, cold (overhead vs SRAM) | +51 % | +28 % | |
| 64 KB working set from flash (mcyc/word, M33) | 81.6 | 53.3 | 0.65x |
| page program 256 B / sector erase avg | 869 us / 6.9 ms | 579 us / 3.9 ms | |

Quad is ~1.5x, not 2x: command and address phases cost the same. Side effect: every
*first-time* execution of SDK code from flash is slower on the black board, so worst
cases that include a cold miss are ~1.6-2x larger there while medians are equal: IRQ
alarm latency max 3302 vs 2013 cycles, software IRQ entry max 1044 vs 624, PIO FIFO round
trip max 481 vs 277, DMA single transfer max 402 vs 210, spinlock max 599 vs 311,
core 1 launch 68 vs 44 us. The one 2414-cycle sample in `hotpath.disturbance`
sram_irqs_10khz on RP2350A M33 is such a cold start (p99.9 is 266 on both boards).

**3. RP2040 is ~1.65x slower per cycle on the hot path** (33 vs 20 cycles, M0+ vs M33)
and its clock is lower, so 248 ns vs 133 ns (120 ns on Hazard3). Its flash (Boya, quad
via SSI at 62.5 MHz) reads 9.6 MB/s uncached, i.e. faster than the RP2350A in dual mode,
and its cold miss (163 cycles at 125 MHz, 1.3 us) is about the RP2350A's in time.

**4. Hazard3 vs M33 on the same RP2350:** Hazard3 wins the hot path (18 vs 20 cycles),
integer ALU (0.88 vs 1.0 cyc/op), multiply, branches, memcpy/memset (~30 % fewer
cycles) and the ARM interpreter (24.4 vs 28.2 cycles per guest instruction). M33 wins
dependent SRAM loads (2.26 vs 3.01 cycles), random LUT lookups (11.1 vs 12.1) and
division (4 vs 19.9 cycles). Hazard3 streams cached flash about twice as fast as the M33
(21.8 vs 11.3 MB/s on the WeAct), probably because of its load pipelining.

**5. ARM interpreter (7 reference games, workload 5354c6e4).** Cycles per guest
instruction: RP2040 42.3, RP2350 M33 28.2 (identical on A and B), RP2350B Hazard3 24.4.
Projected to 250 MHz (above the tested clocks): 5.9 / 8.8 / 10.2 MIPS. Only Boom! (8.9)
and Wizard of Wor (9.0) fit, on Hazard3. The other games need 16-30 MIPS, i.e. 2-3x
what this interpreter gives - a faster executor (predecode / JIT), not a faster clock,
is what the cartridge needs. Running the interpreter on core 1 does not hurt the hot path
on core 0: max 23-25 vs 23 cycles idle (RP2350), 45 vs 40 (RP2040).

**6. PSRAM (WeAct, APS6404 8 MB, GPIO0)** passes integrity over all 8 MB and 9 of 9
timing combinations. Cached it is as fast as SRAM (2.30 cycles per dependent load,
hot path 20 cycles); uncached single read 59 cycles, cache miss 123, stream 27 MB/s read
/ 34 MB/s write. It still hurts flash: a cold flash miss grows from 170 to 514 cycles
(+202 %) while a PSRAM DMA stream runs, and the hot path with its table in flash reaches
max 201 cycles (p99 65) under PSRAM DMA. PSRAM itself barely notices flash traffic (+1 %).

**7. Other notes.** Flash program/erase is slowest on the RP2350A (869 us per page vs 368
on the RP2040 with a Boya part of a newer SFDP revision). The GPIO turnaround test found
no free pin on the black RP2350A (GPIO 28/27/26/22/21/20 all held externally) - worth
checking what the board connects there.

## Recommendation for the cartridge

- Hot path code and tables in SRAM: identical on both RP2350 boards, 120-133 ns.
- Anything that can miss the flash cache inside the hot window costs 1.16 us (quad) or
  1.88 us (dual) per miss: on a dual-mode board, enable quad (set the QE bit, use 0xEB)
  or keep the window free of flash entirely.
- With PSRAM fitted, no PSRAM DMA during the critical window if anything in it comes
  from flash.

## Generated tables (worklog/tools/compare_runs.py)

- **RP2040 purple (M0+)**: RP2040 / Cortex-M0+, build df946782, operating points [48, 96, 120, 125, 133] MHz
- **RP2350A black, dual flash (M33)**: RP2350A / Cortex-M33, build 0b51ebcd, operating points [48, 96, 120, 133, 150] MHz
- **RP2350A black, dual flash (Hazard3)**: RP2350A / Hazard3 (RV32IMAC+), build 711c8a63, operating points [48, 96, 120, 133, 150] MHz
- **WeAct RP2350B quad+PSRAM (M33)**: RP2350B / Cortex-M33, build 0b51ebcd, operating points [48, 96, 120, 133, 150] MHz
- **WeAct RP2350B quad+PSRAM (Hazard3)**: RP2350B / Hazard3 (RV32IMAC+), build 9c1104a6, operating points [48, 96, 120, 133, 150] MHz

### Cartridge hot path - RP2040 purple (M0+) at 133 MHz

| variant | p50 [cyc] | p99.9 [cyc] | max [cyc] | p50 [ns] | max [ns] |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 33 | 40 | 40 | 248.1 | 300.8 |
| table in SRAM, code in flash | 33 | 40 | 743 | 248.1 | 5586.5 |
| table in flash, cache warm | 33 | 84 | 90 | 248.1 | 676.7 |
| table in flash, cache cold | 84 | 90 | 90 | 631.6 | 676.7 |
| table in flash, uncached | 82 | 88 | 88 | 616.5 | 661.7 |

### Cartridge hot path - RP2350A black, dual flash (M33) at 150 MHz

| variant | p50 [cyc] | p99.9 [cyc] | max [cyc] | p50 [ns] | max [ns] |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 20 | 23 | 23 | 133.3 | 153.3 |
| table in SRAM, code in flash | 20 | 23 | 961 | 133.3 | 6406.7 |
| table in flash, cache warm | 20 | 200 | 201 | 133.3 | 1340.0 |
| table in flash, cache cold | 200 | 200 | 200 | 1333.3 | 1333.3 |
| table in flash, uncached | 116 | 117 | 117 | 773.3 | 780.0 |

### Cartridge hot path - RP2350A black, dual flash (Hazard3) at 150 MHz

| variant | p50 [cyc] | p99.9 [cyc] | max [cyc] | p50 [ns] | max [ns] |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 18 | 19 | 19 | 120.0 | 126.7 |
| table in SRAM, code in flash | 17 | 19 | 798 | 113.3 | 5320.0 |
| table in flash, cache warm | 18 | 198 | 199 | 120.0 | 1326.7 |
| table in flash, cache cold | 198 | 198 | 198 | 1320.0 | 1320.0 |
| table in flash, uncached | 114 | 115 | 115 | 760.0 | 766.7 |

### Cartridge hot path - WeAct RP2350B quad+PSRAM (M33) at 150 MHz

| variant | p50 [cyc] | p99.9 [cyc] | max [cyc] | p50 [ns] | max [ns] |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 20 | 23 | 23 | 133.3 | 153.3 |
| table in SRAM, code in flash | 20 | 23 | 553 | 133.3 | 3686.7 |
| table in flash, cache warm | 20 | 140 | 141 | 133.3 | 940.0 |
| table in flash, cache cold | 140 | 140 | 140 | 933.3 | 933.3 |
| table in flash, uncached | 98 | 99 | 99 | 653.3 | 660.0 |
| table in PSRAM, cached | 20 | 87 | 87 | 133.3 | 580.0 |
| table in PSRAM, uncached | 59 | 59 | 59 | 393.3 | 393.3 |

### Cartridge hot path - WeAct RP2350B quad+PSRAM (Hazard3) at 150 MHz

| variant | p50 [cyc] | p99.9 [cyc] | max [cyc] | p50 [ns] | max [ns] |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 18 | 19 | 19 | 120.0 | 126.7 |
| table in SRAM, code in flash | 17 | 19 | 414 | 113.3 | 2760.0 |
| table in flash, cache warm | 18 | 138 | 139 | 120.0 | 926.7 |
| table in flash, cache cold | 138 | 138 | 138 | 920.0 | 920.0 |
| table in flash, uncached | 96 | 97 | 97 | 640.0 | 646.7 |
| table in PSRAM, cached | 18 | 85 | 86 | 120.0 | 573.3 |
| table in PSRAM, uncached | 57 | 58 | 58 | 380.0 | 386.7 |

### Hot path against clk_sys - RP2040 purple (M0+) (cycles / ns)

| variant | 48 MHz | 96 MHz | 120 MHz | 125 MHz | 133 MHz |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 33 / 688 | 33 / 344 | 33 / 275 | 33 / 264 | 33 / 248 |
| table in SRAM, code in flash | 33 / 688 | 33 / 344 | 33 / 275 | 33 / 264 | 33 / 248 |
| table in flash, cache warm | 33 / 688 | 33 / 344 | 33 / 275 | 33 / 264 | 33 / 248 |
| table in flash, cache cold | 84 / 1750 | 84 / 875 | 84 / 700 | 84 / 672 | 84 / 632 |
| table in flash, uncached | 82 / 1708 | 82 / 854 | 82 / 683 | 82 / 656 | 82 / 617 |

### Hot path against clk_sys - RP2350A black, dual flash (M33) (cycles / ns)

| variant | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in SRAM, code in flash | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in flash, cache warm | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in flash, cache cold | 200 / 4167 | 200 / 2083 | 200 / 1667 | 200 / 1504 | 200 / 1333 |
| table in flash, uncached | 116 / 2417 | 116 / 1208 | 116 / 967 | 116 / 872 | 116 / 773 |

### Hot path against clk_sys - RP2350A black, dual flash (Hazard3) (cycles / ns)

| variant | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 18 / 375 | 18 / 188 | 18 / 150 | 18 / 135 | 18 / 120 |
| table in SRAM, code in flash | 17 / 354 | 17 / 177 | 17 / 142 | 17 / 128 | 17 / 113 |
| table in flash, cache warm | 18 / 375 | 18 / 188 | 18 / 150 | 18 / 135 | 18 / 120 |
| table in flash, cache cold | 198 / 4125 | 198 / 2062 | 198 / 1650 | 198 / 1489 | 198 / 1320 |
| table in flash, uncached | 114 / 2375 | 114 / 1188 | 114 / 950 | 114 / 857 | 114 / 760 |

### Hot path against clk_sys - WeAct RP2350B quad+PSRAM (M33) (cycles / ns)

| variant | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in SRAM, code in flash | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in flash, cache warm | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in flash, cache cold | 140 / 2917 | 140 / 1458 | 140 / 1167 | 140 / 1053 | 140 / 933 |
| table in flash, uncached | 98 / 2042 | 98 / 1021 | 98 / 817 | 98 / 737 | 98 / 653 |
| table in PSRAM, cached | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in PSRAM, uncached | 41 / 854 | 42 / 438 | 59 / 492 | 59 / 444 | 59 / 393 |

### Hot path against clk_sys - WeAct RP2350B quad+PSRAM (Hazard3) (cycles / ns)

| variant | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 18 / 375 | 18 / 188 | 18 / 150 | 18 / 135 | 18 / 120 |
| table in SRAM, code in flash | 17 / 354 | 17 / 177 | 17 / 142 | 17 / 128 | 17 / 113 |
| table in flash, cache warm | 18 / 375 | 18 / 188 | 18 / 150 | 18 / 135 | 18 / 120 |
| table in flash, cache cold | 138 / 2875 | 138 / 1438 | 138 / 1150 | 138 / 1038 | 138 / 920 |
| table in flash, uncached | 96 / 2000 | 96 / 1000 | 96 / 800 | 96 / 722 | 96 / 640 |
| table in PSRAM, cached | 18 / 375 | 18 / 188 | 18 / 150 | 18 / 135 | 18 / 120 |
| table in PSRAM, uncached | 39 / 812 | 40 / 417 | 57 / 475 | 57 / 429 | 57 / 380 |

### Access latency per read - RP2040 purple (M0+) (cycles / ns)

| memory | 48 MHz | 96 MHz | 120 MHz | 125 MHz | 133 MHz |
|---|---|---|---|---|---|
| SRAM, dependent load | 3.52 / 73.4 | 3.52 / 36.7 | 3.52 / 29.4 | 3.52 / 28.2 | 3.52 / 26.5 |
| flash cache hit | - | - | - | 12.00 / 96.0 | - |
| flash cache miss (cold) | - | - | - | 163.00 / 1304.0 | - |

### Access latency per read - RP2350A black, dual flash (M33) (cycles / ns)

| memory | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| SRAM, dependent load | 2.26 / 47.1 | 2.26 / 23.6 | 2.26 / 18.9 | 2.26 / 17.0 | 2.26 / 15.1 |
| flash cache hit | - | - | - | - | 7.00 / 46.7 |
| flash cache miss (cold) | - | - | - | - | 282.00 / 1880.0 |

### Access latency per read - RP2350A black, dual flash (Hazard3) (cycles / ns)

| memory | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| SRAM, dependent load | 3.01 / 62.8 | 3.01 / 31.4 | 3.01 / 25.1 | 3.01 / 22.7 | 3.01 / 20.1 |
| flash cache hit | - | - | - | - | 7.00 / 46.7 |
| flash cache miss (cold) | - | - | - | - | 283.00 / 1886.7 |

### Access latency per read - WeAct RP2350B quad+PSRAM (M33) (cycles / ns)

| memory | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| SRAM, dependent load | 2.26 / 47.1 | 2.26 / 23.6 | 2.26 / 18.9 | 2.26 / 17.0 | 2.26 / 15.1 |
| PSRAM cached, dependent load | 2.30 / 47.9 | 2.30 / 24.0 | 2.30 / 19.2 | 2.30 / 17.3 | 2.30 / 15.3 |
| PSRAM uncached, dependent load | 29.17 / 607.7 | 30.17 / 314.2 | 53.17 / 443.1 | 53.17 / 399.8 | 53.17 / 354.5 |
| PSRAM single read, uncached | 35.00 / 729.2 | 36.00 / 375.0 | 59.00 / 491.7 | 59.00 / 443.6 | 59.00 / 393.3 |
| PSRAM cache miss | 93.00 / 1937.5 | 93.00 / 968.8 | 123.00 / 1025.0 | 123.00 / 924.8 | 123.00 / 820.0 |
| flash cache hit | - | - | - | - | 7.00 / 46.7 |
| flash cache miss (cold) | - | - | - | - | 174.00 / 1160.0 |

### Access latency per read - WeAct RP2350B quad+PSRAM (Hazard3) (cycles / ns)

| memory | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| SRAM, dependent load | 3.01 / 62.8 | 3.01 / 31.4 | 3.01 / 25.1 | 3.01 / 22.7 | 3.01 / 20.1 |
| PSRAM cached, dependent load | 3.05 / 63.5 | 3.05 / 31.8 | 3.05 / 25.4 | 3.05 / 22.9 | 3.05 / 20.3 |
| PSRAM uncached, dependent load | 30.04 / 625.7 | 31.03 / 323.2 | 54.03 / 450.2 | 54.03 / 406.2 | 54.03 / 360.2 |
| PSRAM single read, uncached | 34.00 / 708.3 | 35.00 / 364.6 | 58.00 / 483.3 | 58.00 / 436.1 | 58.00 / 386.7 |
| PSRAM cache miss | 93.00 / 1937.5 | 93.00 / 968.8 | 123.00 / 1025.0 | 123.00 / 924.8 | 123.00 / 820.0 |
| flash cache hit | - | - | - | - | 7.00 / 46.7 |
| flash cache miss (cold) | - | - | - | - | 175.00 / 1166.7 |

### Throughput - RP2040 purple (M0+) (MB/s)

| path | 48 MHz | 96 MHz | 120 MHz | 125 MHz | 133 MHz |
|---|---|---|---|---|---|
| SRAM read 32-bit | 54.5 | 109.0 | 136.2 | 141.9 | 150.9 |
| SRAM write 32-bit | 72.3 | 144.6 | 180.8 | 188.3 | 200.4 |
| flash XIP streaming | 3.6 | 7.1 | 8.9 | 9.3 | 9.9 |
| flash uncached | 3.7 | 7.3 | 9.2 | 9.6 | 10.2 |

### Throughput - RP2350A black, dual flash (M33) (MB/s)

| path | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| flash XIP streaming | 2.4 | 4.7 | 5.9 | 6.5 | 7.4 |
| flash uncached | 2.3 | 4.6 | 5.8 | 6.4 | 7.2 |

### Throughput - RP2350A black, dual flash (Hazard3) (MB/s)

| path | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| flash XIP streaming | 3.7 | 7.5 | 9.3 | 10.3 | 11.7 |
| flash uncached | 2.3 | 4.6 | 5.8 | 6.4 | 7.2 |

### Throughput - WeAct RP2350B quad+PSRAM (M33) (MB/s)

| path | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| flash XIP streaming | 3.6 | 7.2 | 9.0 | 10.0 | 11.3 |
| flash uncached | 3.5 | 7.0 | 8.8 | 9.7 | 11.0 |
| PSRAM read uncached | 13.8 | 28.1 | 21.8 | 24.1 | 27.3 |
| PSRAM write uncached | 17.9 | 36.1 | 26.9 | 29.8 | 33.7 |
| PSRAM read cached (4 KB) | 80.3 | 160.6 | 200.8 | 222.5 | 251.0 |

### Throughput - WeAct RP2350B quad+PSRAM (Hazard3) (MB/s)

| path | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| flash XIP streaming | 7.0 | 13.9 | 17.4 | 19.3 | 21.8 |
| flash uncached | 3.5 | 7.0 | 8.8 | 9.8 | 11.0 |
| PSRAM read uncached | 13.9 | 28.3 | 21.9 | 24.3 | 27.5 |
| PSRAM write uncached | 18.3 | 37.0 | 27.2 | 30.2 | 34.2 |
| PSRAM read cached (4 KB) | 84.7 | 169.3 | 211.7 | 234.6 | 264.6 |

### Hot path under load - RP2040 purple (M0+) at 125 MHz

| variant | p50 [cyc] | p99 [cyc] | p99.9 [cyc] | max [cyc] | max [ns] |
|---|---|---|---|---|---|
| SRAM table, quiet | 33 | 33 | 40 | 40 | 320.0 |
| SRAM table, 10 kHz interrupt | 33 | 33 | 328 | 329 | 2632.0 |
| SRAM table, SRAM DMA | 35 | 37 | 41 | 42 | 336.0 |
| flash table, quiet | 33 | 83 | 84 | 90 | 720.0 |
| flash table, SRAM DMA | 34 | 35 | 84 | 87 | 696.0 |

### Hot path under load - RP2350A black, dual flash (M33) at 150 MHz

| variant | p50 [cyc] | p99 [cyc] | p99.9 [cyc] | max [cyc] | max [ns] |
|---|---|---|---|---|---|
| SRAM table, quiet | 20 | 20 | 23 | 23 | 153.3 |
| SRAM table, 10 kHz interrupt | 20 | 20 | 266 | 2414 | 16093.3 |
| SRAM table, SRAM DMA | 20 | 21 | 23 | 23 | 153.3 |
| flash table, quiet | 20 | 20 | 200 | 201 | 1340.0 |
| flash table, SRAM DMA | 20 | 20 | 190 | 200 | 1333.3 |

### Hot path under load - RP2350A black, dual flash (Hazard3) at 150 MHz

| variant | p50 [cyc] | p99 [cyc] | p99.9 [cyc] | max [cyc] | max [ns] |
|---|---|---|---|---|---|
| SRAM table, quiet | 18 | 18 | 19 | 19 | 126.7 |
| SRAM table, 10 kHz interrupt | 18 | 18 | 272 | 274 | 1826.7 |
| SRAM table, SRAM DMA | 18 | 20 | 20 | 20 | 133.3 |
| flash table, quiet | 18 | 18 | 198 | 199 | 1326.7 |
| flash table, SRAM DMA | 18 | 19 | 19 | 199 | 1326.7 |

### Hot path under load - WeAct RP2350B quad+PSRAM (M33) at 150 MHz

| variant | p50 [cyc] | p99 [cyc] | p99.9 [cyc] | max [cyc] | max [ns] |
|---|---|---|---|---|---|
| SRAM table, quiet | 20 | 20 | 23 | 23 | 153.3 |
| SRAM table, 10 kHz interrupt | 20 | 20 | 266 | 269 | 1793.3 |
| SRAM table, SRAM DMA | 20 | 21 | 23 | 23 | 153.3 |
| SRAM table, PSRAM DMA | 20 | 20 | 23 | 23 | 153.3 |
| flash table, quiet | 20 | 20 | 140 | 141 | 940.0 |
| flash table, SRAM DMA | 20 | 20 | 130 | 140 | 933.3 |
| flash table, PSRAM DMA | 22 | 65 | 151 | 201 | 1340.0 |

### Hot path under load - WeAct RP2350B quad+PSRAM (Hazard3) at 150 MHz

| variant | p50 [cyc] | p99 [cyc] | p99.9 [cyc] | max [cyc] | max [ns] |
|---|---|---|---|---|---|
| SRAM table, quiet | 18 | 18 | 19 | 19 | 126.7 |
| SRAM table, 10 kHz interrupt | 18 | 18 | 272 | 274 | 1826.7 |
| SRAM table, SRAM DMA | 18 | 20 | 20 | 20 | 133.3 |
| SRAM table, PSRAM DMA | 18 | 18 | 19 | 20 | 133.3 |
| flash table, quiet | 18 | 18 | 138 | 139 | 926.7 |
| flash table, SRAM DMA | 18 | 19 | 19 | 139 | 926.7 |
| flash table, PSRAM DMA | 18 | 48 | 66 | 178 | 1186.7 |
