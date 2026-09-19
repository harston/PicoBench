# PicoBench work log

Newest entry first. Each entry: what was done, decisions, hardware results, open issues, next step.

---

## 2026-09-19 — Phase 1: core infrastructure

**Done**
- Commit `59e75d1`: phase 0 (the LED sequence works on every board: Pico and purple clone, Pico 2, WeAct RP2350B ARM and RISC-V).
- `src/core/timing.[ch]`: one cycle-counter API. M33 uses DWT CYCCNT, Hazard3 uses `mcycle` (clears `mcountinhibit`), M0+ uses 24-bit SysTick. Read overhead is calibrated and subtracted by `timing_net()`.
- `src/core/stats.[ch]`: exact percentiles via in-place heapsort (no extra RAM). A streaming collector keeps an exact per-cycle histogram below N bins, with log2 buckets above that marked `STATS_APPROX`.
- `src/core/clocks_ctl.[ch]`: operating points (RP2040 48/96/120/125/133, RP2350 48/96/120/133/150 MHz) and full readback: frequency counter for every clock, PLL dividers and lock, clock mux registers, flash interface divider (SSI BAUDR / QMI M0 CLKDIV).
- `src/core/guard.[ch]`: watchdog (5 s) plus stage number in watchdog scratch 0-3.
- `src/results/record.[ch]`: binary TLV records in an arena in `.uninitialized_data` (RP2040 24 KB, RP2350 64 KB). The header is protected by CRC32 and the data by a running CRC32, so the arena can be resumed after a watchdog reset.
- `src/core/sequencer.[ch]` + `src/tests/registry.c`: static test table with stable IDs. Stages are ordered as all tests at the default operating point, then `TF_SWEEP` tests at every other point. The sequencer logs each operating point's readback once and records N/A for chip-mismatched tests.
- `src/core/led.[ch]`: status protocol. TESTING and WRITING use PWM; the PASS/FAIL patterns use a drift-free hardware alarm.
- `src/results/summary.[ch]` (per-subsystem verdicts) and `report_text.[ch]` (text report, the basis for REPORT.TXT). Every cycle statistic is also shown in ns.
- `main.c`: boot flow with crash resume and diagnostic mode. The debug USB CDC console after the run (`d`/`r`/`b`) is temporary until phase 2.
- First tests: `core.timing_counter` (100: counter read cost plus cross-check against the 1 MHz timer), `clocks.readback` (101, sweep), `cpu.loop_smoke` (200, sweep, from SRAM with IRQs off), `debug.crash` (990, crashtest builds only).
- `build.sh`: `PICOBENCH_VARIANT=crashtest`.
- Host unit tests: `worklog/tools/host_tests/run.sh` covers CRC-32 check value, heapsort and percentiles against qsort, and streaming stats. Built with ASan/UBSan. All pass.

**Findings**
- The SDK defines `pll_sys` / `pll_usb` as macros, so they can't be used as struct field names.
- Newlib FILE functions (`fwrite`, `fflush`) don't link with pqt-gcc (it needs `__retarget_lock_*`). Use the SDK stdio API instead (noted in CLAUDE.md).
- RP2350 `SYSINFO.PACKAGE_SEL`: 0 = QFN80 (RP2350B), 1 = QFN60 (RP2350A), per the SDK register header.

**Hardware verification (WeAct RP2350B, Cortex-M33; reports from the user)**
- Normal UF2: 11/11 stages, OVERALL PASS. Measured clk_sys matches exactly at 48/96/120/133/150 MHz (PLL 1/120/6/5, 1/128/4/4, 1/120/6/2, 1/133/6/2, 1/125/5/2). QMI flash CLKDIV = 3 at every point. LED 4.471 Hz at 150 MHz and 4.00 Hz elsewhere, as predicted.
- DWT CYCCNT back-to-back read = 1 cycle (the overhead). `loop100` = 299 cycles at every clock (2.99 cycles/iteration), so perfectly clock-independent from SRAM.
- Counter vs 1 MHz timer: -120 / -153 ppm over 10 ms, about 1.2 µs of window-edge error (timer resolution plus `time_us_64` call cost), not a real clock error.
- Crashtest UF2: `debug.crash` hung, the watchdog reset the chip (reason 0x1), the arena was **resumed from RAM without loss**, `TEST_CRASHED` was recorded, and all sweep stages ran after it. OVERALL FAIL, CORE FAIL (crashed 1). The watchdog + retention design works on RP2350.

**Findings from hardware**
- clk_peri is 150 MHz (= clk_sys) after boot, but `set_sys_clock_khz()` moves it to pll_usb 48 MHz and it stays there even back at 150 MHz. Fixed in phase 2: the default operating point is re-applied at boot, so clk_peri is 48 MHz at every point (consistent comparisons).
- `cpu.loop_smoke` max 348 vs 299 cycles, only in the first sampled test after boot (at 150 MHz on a fresh boot, at 48 MHz after the crash resume). Probably an XIP cache miss of the flash-resident caller (the loop itself is in SRAM). Phase 5 (XIP cold/warm) will quantify this.
- Not yet run on RP2040 and RISC-V; next time.

**Next step**
- Phase 2: flash results store (A/B, CRC, COMMITTED), streaming writer, read-only USB MSC with a virtual FAT, boot mode (build ID / button).

---

## 2026-09-19 — Phase 0: bootstrap

**Done**
- `git init`, `.gitignore` (ignores `build/` and `dist/backup/`; the current UF2s in `dist/` are meant to be tracked).
- CMake skeleton (`CMakeLists.txt`, `cmake/pico_sdk_import.cmake` copied from SDK 2.3.0), custom board header `boards/picobench.h`.
- Generated version header (`src/core/version.h.in`): version, git SHA and dirty flag, build time, platform, 32-bit build ID. The build ID is also embedded as binary info.
- `build.sh`: builds rp2040, rp2350-arm-s and rp2350-riscv with the Arduino pqt-gcc toolchains, copies the results to `dist/picobench_{rp2040,rp2350_arm,rp2350_riscv}.uf2`, and backs up the previous UF2s to `dist/backup/<timestamp>/`.
- `src/core/led.[ch]`: status LED on hardware PWM (phase-correct, zero CPU cost), with `led_reclock()` for clk_sys changes.
- `src/main.c` demo loop: ~4 Hz blink 5 s → solid 2 s → off 1 s → 3× PASS pattern (1 blink per 2 s) → 3× FAIL pattern (2 blinks per 2 s).

**Verification**
- All 3 platforms build with no warnings (`-Wall -Wextra`).
- `picotool info -a` shows the right family ID for each file (rp2040 / rp2350-arm-s / rp2350-riscv), SDK 2.3.0, board `picobench`, and an image_def block on the RP2350 builds.
- Hardware (confirmed by the user): the LED sequence works on every board — Pico/purple clone (RP2040), Pico 2, and WeAct RP2350B (ARM and RISC-V). The W25Q080 boot2 is therefore fine on the purple clone.

**Findings**
- PWM limit: the slowest phase-correct blink is clk_sys / (256·2·65536). That's 4.47 Hz at 150 MHz, 3.73 Hz at 125 MHz, and higher at lower clocks it's not a limit. "About 4 Hz" works at every operating point; the driver clamps and reports the real frequency (`led_actual_freq_mhz`), and it's worth logging it in the report.
- The PASS/FAIL patterns (below 1 Hz) can't be done in PWM alone. They run in software in the idle state, which is allowed because no measurements run then.

**Open issues**
- None blocking.

**Next step**
- Phase 1: timing abstraction, stats, test registry/sequencer, watchdog guard + retention, LED state machine (timer-driven PASS/FAIL), clock operating points.

---

## 2026-09-19 — Project analysis and plan

**Done**
- Analysed the requirements (hardware characterization and benchmark firmware for RP2040 / RP2350A / RP2350B).
- Checked the environment:
  - `picotool` is installed (Homebrew). `cmake` and `ninja` are **missing**.
  - Arduino core `rp2040:rp2040` 5.7.0 bundles pico-sdk 2.3.0 plus ARM/RISC-V gcc (`pqt-gcc`, `pqt-gcc-riscv` 4.1.0), usable through `PICO_TOOLCHAIN_PATH`.
- Looked up WeAct RP2350B Core facts: LED GPIO25, KEY GPIO23, PSRAM CS GPIO0, 16 MB W25Q128 flash.
- Wrote the implementation plan: `worklog/PLAN.md`.
- Created `CLAUDE.md` (project rules, boards, build, LED protocol, architecture).

**Decisions (agreed with the user)**
- Boards: WeAct RP2350B (primary), Pico 2 (RP2350A), RP2040 Pico / purple clone. Pico W variants are out of scope.
- Build system: plain Pico SDK 2.3.0 + CMake + Ninja.
- UF2: separate files per chip/arch first (rp2040, rp2350-arm, rp2350-riscv). A universal UF2 comes in the last phase.
- Boot: the test runs once per new firmware (build ID change) or when the button is held at power-up. Otherwise the device only mounts the report disk.

**Open issues**
- Install the build tools: `brew install cmake ninja`, and clone pico-sdk 2.3.0 to `~/pico/pico-sdk`.
- Before phase 6, confirm the PSRAM part planned for the WeAct (e.g. APS6404L 8 MB) and its safe QMI timing range.

**Next step**
- Phase 0: `git init`, `.gitignore`, CMake skeleton for the 3 platforms, `build.sh`, PWM LED blink on all builds.
