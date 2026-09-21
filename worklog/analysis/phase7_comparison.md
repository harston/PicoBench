# PicoBench comparison

- **RP2040 purple clone**: RP2040 / Cortex-M0+, build efdf54f0, operating points [48, 96, 120, 125, 133] MHz
- **RP2350A clone**: RP2350A / Cortex-M33, build 921a4fd8, operating points [48, 96, 120, 133, 150] MHz
- **WeAct RP2350B + PSRAM**: RP2350B / Cortex-M33, build 921a4fd8, operating points [48, 96, 120, 133, 150] MHz

### Cartridge hot path - RP2040 purple clone at 133 MHz

| variant | p50 [cyc] | p99.9 [cyc] | max [cyc] | p50 [ns] | max [ns] |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 33 | 40 | 40 | 248.1 | 300.8 |
| table in SRAM, code in flash | 33 | 40 | 743 | 248.1 | 5586.5 |
| table in flash, cache warm | 33 | 84 | 91 | 248.1 | 684.2 |
| table in flash, cache cold | 84 | 90 | 90 | 631.6 | 676.7 |
| table in flash, uncached | 82 | 88 | 88 | 616.5 | 661.7 |

### Cartridge hot path - RP2350A clone at 150 MHz

| variant | p50 [cyc] | p99.9 [cyc] | max [cyc] | p50 [ns] | max [ns] |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 20 | 23 | 24 | 133.3 | 160.0 |
| table in SRAM, code in flash | 20 | 23 | 686 | 133.3 | 4573.3 |
| table in flash, cache warm | 20 | 200 | 201 | 133.3 | 1340.0 |
| table in flash, cache cold | 200 | 201 | 201 | 1333.3 | 1340.0 |
| table in flash, uncached | 116 | 117 | 117 | 773.3 | 780.0 |

### Cartridge hot path - WeAct RP2350B + PSRAM at 150 MHz

| variant | p50 [cyc] | p99.9 [cyc] | max [cyc] | p50 [ns] | max [ns] |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 20 | 23 | 24 | 133.3 | 160.0 |
| table in SRAM, code in flash | 20 | 23 | 386 | 133.3 | 2573.3 |
| table in flash, cache warm | 20 | 140 | 141 | 133.3 | 940.0 |
| table in flash, cache cold | 140 | 141 | 141 | 933.3 | 940.0 |
| table in flash, uncached | 98 | 99 | 99 | 653.3 | 660.0 |
| table in PSRAM, cached | 20 | 88 | 88 | 133.3 | 586.7 |
| table in PSRAM, uncached | 59 | 60 | 60 | 393.3 | 400.0 |

### Hot path against clk_sys - RP2040 purple clone (cycles / ns)

| variant | 48 MHz | 96 MHz | 120 MHz | 125 MHz | 133 MHz |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 33 / 688 | 33 / 344 | 33 / 275 | 33 / 264 | 33 / 248 |
| table in SRAM, code in flash | 33 / 688 | 33 / 344 | 33 / 275 | 33 / 264 | 33 / 248 |
| table in flash, cache warm | 33 / 688 | 33 / 344 | 33 / 275 | 33 / 264 | 33 / 248 |
| table in flash, cache cold | 84 / 1750 | 84 / 875 | 84 / 700 | 84 / 672 | 84 / 632 |
| table in flash, uncached | 82 / 1708 | 82 / 854 | 82 / 683 | 82 / 656 | 82 / 617 |

### Hot path against clk_sys - RP2350A clone (cycles / ns)

| variant | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in SRAM, code in flash | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in flash, cache warm | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in flash, cache cold | 200 / 4167 | 200 / 2083 | 200 / 1667 | 200 / 1504 | 200 / 1333 |
| table in flash, uncached | 116 / 2417 | 116 / 1208 | 116 / 967 | 116 / 872 | 116 / 773 |

### Hot path against clk_sys - WeAct RP2350B + PSRAM (cycles / ns)

| variant | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| table in SRAM, code in SRAM | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in SRAM, code in flash | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in flash, cache warm | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in flash, cache cold | 140 / 2917 | 140 / 1458 | 140 / 1167 | 140 / 1053 | 140 / 933 |
| table in flash, uncached | 98 / 2042 | 98 / 1021 | 98 / 817 | 98 / 737 | 98 / 653 |
| table in PSRAM, cached | 20 / 417 | 20 / 208 | 20 / 167 | 20 / 150 | 20 / 133 |
| table in PSRAM, uncached | 41 / 854 | 42 / 438 | 59 / 492 | 59 / 444 | 59 / 393 |

### Access latency per read - RP2040 purple clone (cycles / ns)

| memory | 48 MHz | 96 MHz | 120 MHz | 125 MHz | 133 MHz |
|---|---|---|---|---|---|
| SRAM, dependent load | 3.52 / 73.4 | 3.52 / 36.7 | 3.52 / 29.4 | 3.52 / 28.2 | 3.52 / 26.5 |
| flash cache hit | - | - | - | 12.00 / 96.0 | - |
| flash cache miss (cold) | - | - | - | 163.00 / 1304.0 | - |

### Access latency per read - RP2350A clone (cycles / ns)

| memory | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| SRAM, dependent load | 2.26 / 47.1 | 2.26 / 23.6 | 2.26 / 18.9 | 2.26 / 17.0 | 2.26 / 15.1 |
| flash cache hit | - | - | - | - | 7.00 / 46.7 |
| flash cache miss (cold) | - | - | - | - | 283.00 / 1886.7 |

### Access latency per read - WeAct RP2350B + PSRAM (cycles / ns)

| memory | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| SRAM, dependent load | 2.26 / 47.1 | 2.26 / 23.6 | 2.26 / 18.9 | 2.26 / 17.0 | 2.26 / 15.1 |
| PSRAM cached, dependent load | 2.30 / 47.9 | 2.30 / 24.0 | 2.30 / 19.2 | 2.30 / 17.3 | 2.30 / 15.3 |
| PSRAM uncached, dependent load | 29.17 / 607.7 | 30.17 / 314.2 | 53.17 / 443.1 | 53.17 / 399.8 | 53.17 / 354.5 |
| PSRAM single read, uncached | 34.00 / 708.3 | 35.00 / 364.6 | 58.00 / 483.3 | 58.00 / 436.1 | 58.00 / 386.7 |
| PSRAM cache miss | 94.00 / 1958.3 | 94.00 / 979.2 | 124.00 / 1033.3 | 124.00 / 932.3 | 124.00 / 826.7 |
| flash cache hit | - | - | - | - | 7.00 / 46.7 |
| flash cache miss (cold) | - | - | - | - | 175.00 / 1166.7 |

### Throughput - RP2040 purple clone (MB/s)

| path | 48 MHz | 96 MHz | 120 MHz | 125 MHz | 133 MHz |
|---|---|---|---|---|---|
| SRAM read 32-bit | 54.5 | 109.0 | 136.2 | 141.9 | 150.9 |
| SRAM write 32-bit | 72.3 | 144.6 | 180.8 | 188.3 | 200.4 |
| flash XIP streaming | 3.6 | 7.1 | 8.9 | 9.3 | 9.9 |
| flash uncached | 3.7 | 7.3 | 9.2 | 9.6 | 10.2 |

### Throughput - RP2350A clone (MB/s)

| path | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| SRAM read 32-bit | 76.4 | 152.7 | 190.9 | 211.6 | 238.6 |
| SRAM write 32-bit | 117.3 | 234.6 | 293.3 | 325.0 | 366.6 |
| flash XIP streaming | 2.4 | 4.7 | 5.9 | 6.5 | 7.4 |
| flash uncached | 2.3 | 4.6 | 5.8 | 6.4 | 7.2 |

### Throughput - WeAct RP2350B + PSRAM (MB/s)

| path | 48 MHz | 96 MHz | 120 MHz | 133 MHz | 150 MHz |
|---|---|---|---|---|---|
| SRAM read 32-bit | 76.4 | 152.7 | 190.9 | 211.6 | 238.6 |
| SRAM write 32-bit | 117.3 | 234.6 | 293.3 | 325.0 | 366.6 |
| flash XIP streaming | 3.6 | 7.2 | 9.0 | 10.0 | 11.3 |
| flash uncached | 3.5 | 7.0 | 8.8 | 9.7 | 11.0 |
| PSRAM read uncached | 13.8 | 28.1 | 21.8 | 24.1 | 27.3 |
| PSRAM write uncached | 17.9 | 36.1 | 26.9 | 29.8 | 33.7 |
| PSRAM read cached (4 KB) | 80.3 | 160.6 | 200.8 | 222.5 | 251.0 |

### Hot path under load - RP2040 purple clone at 125 MHz

| variant | p50 [cyc] | p99 [cyc] | p99.9 [cyc] | max [cyc] | max [ns] |
|---|---|---|---|---|---|
| SRAM table, quiet | 33 | 33 | 40 | 40 | 320.0 |
| SRAM table, 10 kHz interrupt | 33 | 33 | 328 | 335 | 2680.0 |
| SRAM table, SRAM DMA | 33 | 34 | 41 | 42 | 336.0 |
| flash table, quiet | 33 | 83 | 84 | 90 | 720.0 |
| flash table, SRAM DMA | 33 | 34 | 83 | 85 | 680.0 |

### Hot path under load - RP2350A clone at 150 MHz

| variant | p50 [cyc] | p99 [cyc] | p99.9 [cyc] | max [cyc] | max [ns] |
|---|---|---|---|---|---|
| SRAM table, quiet | 20 | 22 | 23 | 24 | 160.0 |
| SRAM table, 10 kHz interrupt | 20 | 22 | 266 | 2320 | 15466.7 |
| SRAM table, SRAM DMA | 20 | 22 | 23 | 24 | 160.0 |
| flash table, quiet | 20 | 21 | 200 | 201 | 1340.0 |
| flash table, SRAM DMA | 20 | 21 | 190 | 201 | 1340.0 |

### Hot path under load - WeAct RP2350B + PSRAM at 150 MHz

| variant | p50 [cyc] | p99 [cyc] | p99.9 [cyc] | max [cyc] | max [ns] |
|---|---|---|---|---|---|
| SRAM table, quiet | 20 | 22 | 23 | 24 | 160.0 |
| SRAM table, 10 kHz interrupt | 20 | 22 | 266 | 268 | 1786.7 |
| SRAM table, SRAM DMA | 20 | 22 | 23 | 24 | 160.0 |
| SRAM table, PSRAM DMA | 20 | 22 | 23 | 25 | 166.7 |
| flash table, quiet | 20 | 21 | 140 | 141 | 940.0 |
| flash table, SRAM DMA | 20 | 21 | 130 | 140 | 933.3 |
| flash table, PSRAM DMA | 22 | 60 | 149 | 183 | 1220.0 |
