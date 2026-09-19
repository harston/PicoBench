# PicoBench — Pico Hardware Characterization & Benchmark: plan

## Context
The `PicoBench/` directory is empty. The goal is self-contained firmware (one UF2 per chip) that runs the full hardware characterization and benchmark suite (spec pasted by the user) on RP2040 and RP2350A/B boards. It needs no external equipment. Results go to a dedicated power-loss-safe flash area. The LED reports progress and the final verdict. When the run ends, the device mounts as a read-only USB disk with the reports. The main consumer of the data is the Atari cartridge work (PicoA10400 / Pico2A10400), so worst-case latency (max, p99.9) matters more than averages.

Decisions made with the user:
- **Boards:** WeAct RP2350B (primary: 16 MB W25Q128, LED GPIO25, KEY GPIO23, optional PSRAM with CS on GPIO0), Pico 2 (RP2350A, 4 MB, LED GPIO25), RP2040 Pico / purple clone (2–16 MB, LED GPIO25). Pico W boards are out of scope.
- **Build:** plain Pico SDK 2.3.0 + CMake + Ninja (not Arduino).
- **UF2:** separate files first (`picobench_rp2040.uf2`, `picobench_rp2350_arm.uf2`, `picobench_rp2350_riscv.uf2`). A combined "universal" UF2 comes in the last phase.
- **Boot behaviour:** the test runs once after a new firmware is flashed (its build ID differs from the last stored run) or when the button is held at power-up (BOOTSEL on Pico/Pico 2; KEY GPIO23 or BOOTSEL on WeAct). Every other boot only mounts the report disk.

## Environment (verified)
- `picotool` is installed (`/opt/homebrew/bin/picotool`). `cmake` and `ninja` are missing: `brew install cmake ninja`.
- The Arduino core `rp2040:rp2040` 5.7.0 bundles pico-sdk **2.3.0** (`~/Library/Arduino15/packages/rp2040/hardware/rp2040/5.7.0/pico-sdk`, with `hardware_psram`, `hardware_xip_cache`, `hardware_sha256`, `pico_rand`, and so on) plus ARM and RISC-V gcc (`~/Library/Arduino15/packages/rp2040/tools/pqt-gcc{,-riscv}/4.1.0-1aec55e`). We use a clean `git clone -b 2.3.0 --recurse-submodules` of pico-sdk (at `~/pico/pico-sdk`, set via `PICO_SDK_PATH`) so the build doesn't depend on Arduino patches. The bundled pqt-gcc toolchains can be reused through `PICO_TOOLCHAIN_PATH`, which avoids a separate ARM/RISC-V install.

## Architecture

```
PicoBench/
  CLAUDE.md                 project rules + hardware facts (English)
  CMakeLists.txt            one project, 3 build dirs: -DPICO_PLATFORM=rp2040 | rp2350-arm-s | rp2350-riscv, PICO_BOARD=none/custom
  build.sh                  builds all variants, copies UF2s to dist/, backs up previous UF2s first
  boards/picobench_board.h  custom board header (no LED/flash assumptions; everything detected at runtime)
  src/
    main.c                  boot flow: retention check → decide run/skip → test sequencer → report → MSC
    core/                   timing.[ch] (cycle counter abstraction), stats.[ch] (min/median/p95/p99/p99.9/max, histograms),
                            clocks_ctl.[ch] (safe operating points), led.[ch], watchdog_guard.[ch], retention.[ch], platform.[ch]
    detect/                 chip.c (RP2040/RP2350A/B, revision, arch), board.c (profile), flash_id.c (JEDEC, SFDP size, unique ID),
                            psram_detect.c, bootrom.c
    tests/                  one file per subsystem: cpu.c, sram_integrity.c, sram_bench.c, contention.c, flash_xip.c, cache.c,
                            psram_integrity.c, psram_bench.c, psram_timing.c, qmi_conflict.c, hotpath.c (+ hotpath.pio),
                            pio_bench.c (+ .pio), sio_gpio.c, dma.c, multicore.c, irq_timer.c, crypto_misc.c, adc_temp.c, flash_write.c
    results/                record.[ch] (binary TLV result store in RAM), writers: report_txt.c, csv.c, json.c, hardware_txt.c, errors_txt.c, raw_bin.c
    storage/                results_store.[ch] (A/B slots + run history in flash), stream_writer (4 KB page buffer → flash)
    usb/                    tusb_config.h, usb_descriptors.c, msc_disk.c (read-only virtual FAT, files mapped straight onto flash)
  worklog/WORKLOG.md        running work log; worklog/ also holds scripts, analyses, host tools
  worklog/tools/            host-side Python: parse RESULTS.JSON, compare runs (before/after PSRAM), plot histograms
```

### Key design points
1. **Test registry + sequencer.** Each test is a `{id, name, subsystem, chip_mask, risk_level, fn}` entry in a static table. Before each test the sequencer writes the stage ID and boot counter to watchdog scratch (RP2040: scratch0–3; RP2350: scratch0–3, since 4–7 are used by the bootrom) and arms the watchdog. After a reset it logs `TEST_CRASHED(stage)` in ERRORS, skips that test and continues. If the same stage crashes twice, the run finishes in "diagnostic report" mode.
2. **Timing abstraction:** RP2350-ARM uses DWT CYCCNT, RP2350-RISCV uses `mcycle`, RP2040 uses SysTick (24-bit, clk_sys, reloaded). Long-duration timing uses the 1 µs `time_us_64` timer. Every result is stored in cycles, and ns are derived from the measured clk_sys.
3. **Statistics:** per-sample buffer (up to N = 64 k uint32) → sort → min/p50/p95/p99/p99.9/max + log2 histogram, plus a linear histogram for the hot path. Every table records iterations and buffer sizes.
4. **Operating points:** RP2350 at 48/96/120/133/150 MHz, RP2040 at 48/96/120/125/133 MHz, using `set_sys_clock_khz`. At each point we re-read the real configuration (`frequency_count_khz` for clk_sys/ref/peri/usb/adc, PLL registers: refdiv, fbdiv, postdiv1/2, sources) and run the "key subset" of benchmarks (CPU, SRAM, XIP warm/cold, PSRAM, hot path). The full suite runs at the default clock (150/125 MHz). No overclocking.
5. **LED without CPU cost:** LED GPIO25 is driven by hardware PWM (PWM slice 4B). Patterns: ~4 Hz blinking = testing; solid on = writing/verifying flash; 1 short blink every 2 s = PASS; 2 short blinks every 2 s = FAIL, report saved. Double-blink patterns are made with PWM wrap + a slow alarm only in the final idle state, never during measurements. The PWM divider is recomputed after every clock change. The LED pin comes from the detected board profile. On an unknown board it falls back to GPIO25.
6. **Board detection:** chip from SYSINFO `CHIP_ID` / `PACKAGE_SEL` (RP2350A vs B, 30 vs 48 GPIO), silicon revision, flash JEDEC ID and SFDP size, 64-bit unique ID. Profile rules: RP2350B + 16 MB → WeAct; RP2350A → Pico 2; RP2040 → Pico/clone (flash size logged). PSRAM is probed on every pin that has the XIP_CS1 function (RP2350: GPIO 0, 8, 19, 47; 47 only on B), trying the WeAct GPIO0 first, via `hardware_psram` detection (read ID 0x9F, KGD, size), all wrapped by the watchdog.
7. **Memory tests on SRAM that's in use:** the firmware's own RAM layout is kept small and confined to known banks. Free banks are tested destructively. Occupied regions are tested chunk-by-chunk with save/test/restore, with interrupts off and the test routine running from a different bank (or scratch_x/y) using registers only. On RP2350, banks 0–3 and 4–7 are word-striped, so each "bank" is selected with a 16-byte stride and the two 4 KB banks (8 and 9) are tested separately. On RP2040 the non-striped alias at 0x21000000 is used.
8. **Results pipeline:** tests only append binary records (TLV: test id, variant, clock point, unit, stats, histogram) to a RAM arena. At the end the writers stream REPORT.TXT, RESULTS.CSV, RESULTS.JSON, HARDWARE.TXT, ERRORS.TXT and RAW.BIN straight into flash through a 4 KB buffer, so no whole file ever sits in RAM (important for RP2040's 264 KB).
9. **Flash results store (end of flash):**
   - Layout: `[history area: RUN slots][slot A header][slot B header]`, sized to the flash (for example 1 MB on 4 MB flash, 4 MB on 16 MB, 512 KB on 2 MB). The firmware reserves the area in its linker script, so the binary can never overlap it.
   - Each run record holds magic, format version, run number, boot counter, build ID, length, per-file table, CRC32 over the payload, and a final COMMITTED word written last.
   - Protocol: write to the inactive slot → read back and verify CRC → write COMMITTED → only then does the previous slot become the "older" one. Power loss mid-write leaves the previous report intact.
   - Keeps the last K runs (RUN0001…); the oldest is erased when space runs out.
10. **USB MSC only after the run.** TinyUSB device MSC (read-only, volume label `PICOBENCH`), VID/PID from the RPi test range. It's a virtual FAT12/16 image built from the directory of committed runs: root holds the latest run's REPORT.TXT and other files, plus `RUNS/RUN0001/…`. Sector reads map directly to XIP flash addresses. `tud_msc_is_writable_cb` returns false. USB isn't started during measurements. The one exception is the "USB interrupt impact" test, which briefly brings up a CDC device.
11. **Verdict per subsystem** (CPU, SRAM, FLASH, PSRAM: PASS/NOT PRESENT/FAIL, DMA, PIO, MULTICORE, IRQ, USB, CRYPTO, ADC) with error counts. It sits at the end of REPORT.TXT, next to the full tables.

## Test catalogue (chip applicability)
Implemented as in the spec. Notes on RP2040-specific differences:
- Inventory: RP2040 = 264 KB SRAM in 6 banks, 12 DMA channels, 2 PIO, no PSRAM/SHA/TRNG/QMI (SSI instead of QMI, XIP cache 16 KB with flush). Tests that don't apply write `N/A` records rather than being hidden.
- CPU (per core): loops, branches, int arithmetic, shifts, LUT, memcpy, memset, CRC32 (SW), divider (RP2040 SIO divider vs M33 UDIV), FPU/DCP/DSP (RP2350).
- SRAM integrity (walking 1/0, solid 0x00/FF/55/AA, address, ~address, LFSR, March C-) and SRAM bench (8/16/32-bit, seq/rev/random/stride, aligned/misaligned, per-bank).
- Contention: CPU0 only; CPU0 + busy CPU1; CPU + DMA; both cores on the same bank vs different banks; code in bank X while DMA hits bank Y.
- Flash/XIP: size and IDs, QMI/SSI config, 8/16/32 seq/random, block sizes, flash→SRAM copy, code executing from XIP warm/cold, cached vs uncached alias, cache invalidate/flush cost, XIP cache used as RAM (RP2350, a separate mode).
- PSRAM (RP2350 only, when detected): full integrity (walking, patterns, address, LFSR, power-of-2 boundaries, first/last byte per region, large blocks, multiple passes); benchmark cached and uncached; SRAM↔PSRAM↔PSRAM copies; limited QMI timing sweep (clock divider/RX delay only within the SDK's safe profile, with a watchdog, and stable/errors/timeout per point); QMI conflict tests with flash.
- Cartridge hot path: address → bank/offset → byte → prepared output. Millions of addresses (sequential, random, bank-switch-heavy patterns). Variants: LUT in SRAM/PSRAM/Flash, code in XIP vs SRAM, CPU0 alone vs with CPU1 active, IRQs on/off, PIO-only, CPU+PIO, DMA-assisted. Output min/p50/p95/p99/p99.9/max in ns and cycles, plus a histogram.
- PIO/SIO/GPIO: all PIO blocks and SMs, FIFO push/pull, PIO→CPU IRQ, DMA→PIO, CPU→PIO, max sustained FIFO throughput; SIO set/clr/xor/read, direction switching, CPU/PIO→GPIO internal loopback via input readback. The report states explicitly: "internal timing only, not electrical".
- DMA: each channel sanity-checked; SRAM↔SRAM/PSRAM, Flash→SRAM, SRAM→PIO; sizes, chaining, pacing (timer DREQ), parallel channels.
- Multicore: core1 launch time, FIFO round-trip, spinlock and mutex cost, core0↔core1 event latency and jitter.
- IRQ/timer: ISR entry/exit, alarm latency, nesting, impact of DMA/USB/core1, interrupt storm with the worst case recorded.
- Crypto/misc: SHA-256 HW vs SW, TRNG sanity (monobit/runs), FPU/DCP.
- ADC: temperature sensor and noise on internal channels (a sanity check, not a calibration).
- Flash write: scratch area of a few sectors; program/erase/read/verify timings.

## Implementation phases (each ends with a flashable UF2 and a WORKLOG entry)
0. **Bootstrap:** create CLAUDE.md, worklog/WORKLOG.md, .gitignore, `git init`, CMake skeleton for the 3 platforms, build.sh; blink the LED via PWM on all 3 builds.
1. **Core infrastructure:** timing, stats, registry/sequencer, watchdog guard + retention, LED state machine, clock operating points.
2. **Storage + USB:** results store A/B with CRC/COMMITTED, streaming writer, virtual FAT MSC, boot-mode logic (build ID / button). This comes early so every later phase is visible on the disk.
3. **Inventory + HARDWARE.TXT:** chip, board, flash, bootrom, SDK version, clocks, QMI/SSI, PSRAM detection.
4. CPU, SRAM integrity/bench, contention.
5. Flash/XIP/cache, flash write test.
6. PSRAM integrity/bench/timing sweep, QMI conflict (needs PSRAM on the WeAct to verify the full path).
7. Cartridge hot path (the most important for Atari).
8. PIO/SIO/GPIO, DMA, multicore, IRQ/timer.
9. Crypto/TRNG/FPU, ADC/temperature, verdicts, polished REPORT.TXT/JSON/CSV.
10. Host tools in `worklog/tools/` (run comparison, plots); universal UF2 (pico-examples `universal` approach).

## CLAUDE.md content (to be created in phase 0, in English)
- Purpose and the no-equipment constraint; supported boards table (chip, flash, LED, button, PSRAM CS).
- Build: `brew install cmake ninja`, `PICO_SDK_PATH`, `./build.sh` → `dist/*.uf2`; back up previous UF2s before overwriting.
- Hard rules: all comments and docs in English; USB is off during measurements; no overclocking above 150/133 MHz; every risky test wrapped by watchdog + retention stage; never write to flash outside the results/scratch area; the results area is reserved in the linker script; MSC stays read-only; store raw cycles, derive ns; report max/p99.9 always, not just means; mark internal vs electrical timing.
- Architecture summary (sequencer, record store, writers, flash layout), how to add a test.
- Worklog rule: record progress in `worklog/WORKLOG.md` (date, what was done, hardware results, open issues), put scripts and analyses in `worklog/`.

## Verification
- Every phase: `./build.sh` builds all 3 UF2s without warnings; `picotool info -a dist/*.uf2` shows correct family/binary info.
- On hardware (user flashes via BOOTSEL): LED pattern sequence observed; after the run, the PICOBENCH disk mounts on macOS with the expected files; `worklog/tools/validate_run.py` checks the JSON schema and CRC, and compares HARDWARE.TXT with the known board facts.
- Robustness: pull USB power during the "solid LED" phase → the next boot must still show the previous run intact. A forced crash test stage (debug build flag) must produce TEST_CRASHED and let the run continue.
- The second boot without the button → disk only, no re-test. Holding the button → a new RUN000N appears.
- WeAct before/after PSRAM soldering: the comparison tool diffs the two runs.
