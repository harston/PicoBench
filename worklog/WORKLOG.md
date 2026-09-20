# PicoBench work log

Newest entry first. Each entry: what was done, decisions, hardware results, open issues, next step.

---

## 2026-09-20 — Second board: Pico 2 clone (RP2350A), findings and fixes

**Hardware (user, /Volumes/Public/PicoBench/result2, RUN0001, OVERALL PASS)**
- Chip: **RP2350-A2** (CHIP_ID 0x20004927, PACKAGE_SEL=1 -> QFN60/30 GPIO), **boot ROM version 2** - older silicon than the WeAct (A3, ROM 4). Same gitref 0x5a09d5a2.
- Flash: **Boya BY25Q128, 16 MB** (JEDEC `68 40 18`), SFDP rev 1.0, 2 parameter headers, BFPT 9 dwords; density matches; no address wrap.
- **The boot ROM set up XIP in DUAL mode on this board**: read command 0xBB, widths 1-2-2, 0 dummy bits, and **SR2 QE = 0** (quad not enabled). The WeAct (XMC) runs quad: 0xEB, 1-4-4, 16 dummy bits. Same CLKDIV 3 = 50 MHz SCK, so this board's XIP read bandwidth is roughly half. This matters for the cartridge hot path and will show up in the phase 5 flash benchmarks - keep both boards in the comparison.
- PSRAM: correctly probed only GPIO 0/8/19 (no GPIO47 on the A package), all silent -> NOT PRESENT.

**Bugs found and fixed**
- **Temperature read 411 degC (ADC raw 55).** The firmware is compiled for RP2350B, where the temperature sensor is ADC channel 8; on RP2350A it is channel 4, so the compiled `ADC_TEMPERATURE_CHANNEL_NUM` read a non-existent channel. The channel is now chosen from the chip detected at runtime, the channel number is recorded, and an implausible value (outside -40..125 degC) is reported as an error instead of a nonsense number. Other compile-time RP2350B assumptions must be treated the same way - phase 7 GPIO tests must use the runtime GPIO count.
- Board profile said "unknown RP2350A board" for a clone with 16 MB. Now "Pico 2 compatible board / clone (RP2350A)"; the RP2350B fallback keeps the KEY/PSRAM hints.
- `flash.interface` now states the XIP mode explicitly (`quad (1-4-4)` / `dual (1-2-2)` / single), so a dual-mode board is obvious at a glance.
- `PICO_RP2350_A2_SUPPORTED=1` is now set: A2 silicon is in the field, and the SDK uses this for the E14 (QMI/PSRAM) workaround. A2 also gets an errata note in HARDWARE.TXT.
- False alarm: `core.ids` looked empty in my first look at the file; the record is present (my sed range was wrong).

---

## 2026-09-20 — Fix: one button press produced two runs

**Report (user, WeAct RP2350B)**
- Every scan added two RUNxxxx directories instead of one.

**Cause**
- `button_init()` sampled the KEY idle level at boot to stay polarity-agnostic. The re-run press is normally still held while the board reboots, so the "pressed" level was recorded as idle; releasing the key then looked like a new press and triggered a second run 1 s later. The next boot sampled the correct idle level, which is why it was exactly two runs. A long BOOTSEL press had the same effect (hold 1 s -> reboot -> still held -> counted again).

**Fix**
- `button_pressed()` now requires a release before a press counts (`s_armed`), so a button held at boot is ignored until let go.
- KEY GPIO23 is handled as a normal active-low button with pull-up (the user's earlier test confirmed KEY triggers a re-run, which matches that wiring), instead of guessing polarity at boot. If the pin stays low for 3 s after boot it is treated as not-a-button and ignored, so an unknown board cannot loop.
- HARDWARE.TXT gains `key_button` (live state) and `rerun_trigger`.

**Next step**
- User re-tests: one press = exactly one new RUNxxxx.

---

## 2026-09-19 — Phase 3: hardware inventory + HARDWARE.TXT

**Done**
- Commit `4316ed3`: phase 2 (verified on WeAct: RUN0001 on the disk, run-once logic, STATUS.TXT).
- New test flag `TF_INVENTORY`. Inventory stages run first, at the default clock; each records facts only.
  - `core.chip` (110): CHIP_ID fields, silicon (RP2040-Bx / RP2350-Ax), package + "48 GPIO confirmed", SRAM size and banks, DMA/PIO counts, RP2350 gitref, ROM sys_info (chip info words, OTP critical flags, CPU arch), running core.
  - `core.firmware` (111): version, build ID/time, platform, SDK, gcc, boot ROM version, RP2350 boot info words.
  - `core.ids` (112): unique ID (RP2350 from OTP; RP2040 = flash UID).
  - `core.board` (113): inferred profile (`src/detect/board.[ch]`) from chip + flash size: WeAct RP2350B / Pico 2 / Pico / large-flash RP2040 / unknown. Includes the LED, KEY and PSRAM CS hint.
  - `core.memory` (114): firmware size vs 1 MB limit, .data/.bss, heap, both stacks, arena, flash layout. FAIL if the layout is invalid.
  - `core.reset` (115): decoded chip reset reasons (RP2040 vreg_and_chip_reset, RP2350 powman) + watchdog reason.
  - `core.environment` (116): internal temperature (64-sample average, 3.3 V ref assumed), VREG vsel → mV, VSYS via the GPIO29 /3 divider (not on RP2350B).
  - `clocks.sources` (102): XOSC nominal/measured, ROSC and LPOSC measured, source mux names for every clock, both PLLs, operating points.
  - `flash.identify` (400): JEDEC + vendor table, SFDP (header, BFPT, density, cross-check with JEDEC → FAIL on mismatch), SR1-3 (QE, block protect), flash unique ID.
  - `flash.interface` (401): RP2040 SSI (divider, SCK, RX sample delay, frame format, XIP cmd, wait cycles) / RP2350 QMI M0+M1 (timing fields, SCK, read format decode, ATRANS) + XIP cache state.
  - `psram.detect` (500, RP2350, TF_RISKY): probes XIP_CS1 GPIOs in order board hint (WeAct GPIO0) → 0/8/19/47 (47 only on B), with raw MFID/KGD/EID per probe. On success: SDK default init (133 MHz / 8 µs / 18 ns) + first/last word sanity via the uncached alias 0x15000000. Returns N/A when absent.
  - `usb.config` (910).
- HARDWARE.TXT writer (`src/results/report_hardware.c`), added to the stored files and README.
- REPORT.TXT header: board line. Verdict: PSRAM shows "NOT PRESENT" instead of N/A.
- Sizes: text 79-99 KB, bss RP2040 77 KB / RP2350 150 KB.

**Flash size safety (user question: clones may have up to 16 MB)**
- Size detection is read-only: JEDEC 0x9F and SFDP 0x5A, cross-checked against each other. Nothing is written or erased outside the results area (and the 64 KB scratch in phase 5); the firmware area is never touched.
- Real risk found: counterfeit/mislabelled chips (common on clones) claim more than they have, and their addresses wrap. A results area at 12 MB on a chip that is really 2 MB would erase the firmware at offset 0.
- Added an address-wrap probe (read-only): compare 256 bytes at offset 0 with each power-of-two offset from 256 KB up to the claimed size. A match means the chip is that small, and the smaller size is used for the layout. Plus a final check of the results area itself: if it aliases the firmware, `flash_layout()->valid` is false and saving is disabled instead of destroying the firmware.
- Reported in HARDWARE.TXT (`address_wrap`, `size_used`, FAIL on overstatement) and as a warning in STATUS.TXT.
- False positive is possible if identical 256-byte content really exists at a power-of-two offset; the effect is only a smaller usable area, and it is visible in the report.
- PSRAM size comes from the EID (SDK mapping) plus the first/last word sanity check. PSRAM holds nothing persistent, so probing it cannot lose data; phase 6 will add a wrap check there too.

**Hardware verification (WeAct RP2350B, ARM, RUN0010 - full flash contents from the user)**
- OVERALL PASS, 23 stages, 179 records, 10 KB of 64 KB arena. Verdicts: CORE/CLOCKS/CPU/FLASH/USB PASS, PSRAM NOT PRESENT.
- Chip: RP2350B, CHIP_ID 0x30004927, silicon **RP2350-A3**, bootrom version 4, PACKAGE_SEL=0 (QFN80, 48 GPIO), gitref 0x5a09d5a2, OTP critical flags 0 (no secure boot), ROM says Arm.
- ROM CHIP_INFO words 2/3 are the two halves of the unique ID (C79516F9DD83F1F2), so they are device ID / wafer ID - labels fixed accordingly.
- Flash: JEDEC `20 40 18` = XMC/Micron class, 16 MB; SFDP rev 1.6, 3 parameter headers, BFPT 16 dwords at 0x30, density matches JEDEC; address-wrap probe clean up to 16 MB; SR2 QE=1 (quad enabled), no block protection; flash UID 1f1c0f1a1023e742.
- QMI: CLKDIV 3 -> **SCK 50 MHz at 150 MHz clk_sys**, read cmd 0xEB with 1-4-4 widths, 16 dummy bits + mode byte, RX delay 2, cooldown 1, pagebreak 2, min_deselect 7. ATRANS: 4 MB windows, identity. XIP cache enabled (secure and non-secure).
- PSRAM: all four candidate pins answer 00/00/00 -> NOT PRESENT, as expected before soldering.
- Clocks: XOSC 12 MHz exact, ROSC 45.4 MHz, LPOSC 31 kHz, clk_peri/usb/adc all 48 MHz from pll_usb, clk_hstx 150 MHz. PLL_SYS 1500 MHz VCO /5/2, PLL_USB 1200 MHz /5/5.
- Environment: 31.3 degC, VREG vsel 11 = 1.10 V.
- Reset: chip_reset 0x40000 = HAD_RUN_LOW ("RUN pin") with watchdog_reason 0x1, i.e. the re-run's watchdog reboot appears as a chip-level RUN reset on RP2350. A note now says so.
- Fixed after review: core1 stack printed as an empty range (pico_multicore not linked yet -> now says so); QMI M1 labelled "PSRAM (CS1)" even with no PSRAM (now "unused, boot defaults").

**Follow-up on the button fix**
- HARDWARE.TXT showed `key_button: reads low (pressed)`, recorded while the KEY was still held from the re-run request. If GPIO23 turned out to idle low on this board, the first version of the fix would have blocked BOOTSEL too (a single shared "armed" flag). Buttons are now armed independently, and an idle-low KEY is only reported, never acted on., A-rev, XMC flash 16 MB with SFDP matching, QMI decode, PSRAM NOT PRESENT (probe GPIO0: FF/00 bytes), board = WeAct.
- Pico 2 and RP2040: first run of phases 2-3 on these boards (flash writes via SSI on RP2040!).

---

## 2026-09-19 — Phase 2 fix: reports were never saved; writable disk

**Hardware report (user, WeAct RP2350B, ARM)**
- The PICOBENCH disk mounted, but it held only README.TXT and an empty RUNS/. After a restart or KEY press there was a ~5-6 s pause, then the same content again. KEY therefore works (it triggered a re-run).

**Root cause**
- `boards/picobench.h` had `PICO_FLASH_SIZE_BYTES = 2 MB` (meant as "smallest board"). The SDK's `flash_range_erase/program` `hard_assert(offs + count <= PICO_FLASH_SIZE_BYTES)`, which is active in release builds. On the WeAct (16 MB) the results area is at 12 MB, so the first erase panicked. The watchdog reset the chip (the 5 s pause), the save stage was retried, and after the second crash it was skipped by design.
- On RP2350 the same define also sets the FLASH_DEVINFO CS0 size.
- The read-only disk had nothing to do with it (saving happens before USB starts).
- The host tests missed it because the flash stub had no size check.

**Fix**
- `PICO_FLASH_SIZE_BYTES` = 16 MB (largest supported chip). `_Static_assert` in flash_layout.c, and the layout is marked invalid if the detected flash is larger than the define.
- The host flash stub now enforces the same assert. A new test `test_16mb_board` (WeAct layout, results at 0xC00000) passes with 16 MB and **reproduces the failure with the old 2 MB value** ("erase 0xc02000 > PICO_FLASH_SIZE_BYTES").

**User request: disk not read-only, runs deletable**
- MSC is now writable. Host writes go to a RAM overlay (the measurement sample buffer is reused), and the host sees a consistent disk.
- Deleting `RUNS\RUNxxxx` (or `RUNS`) deletes the run(s) from flash via `store_delete_runs()` (A/B commit, power-loss safe), applied 300 ms after the host goes quiet. Detection: the run directory's cluster no longer has a live entry in RUNS (so renaming is not deletion). Other writes (e.g. copying files onto the disk) last until reset. Documented in README.TXT and STATUS.TXT.
- Hidden `.Trashes` (a file: Finder deletes immediately), `.metadata_never_index` and `.fseventsd/no_log` (as CircuitPython does), with VFAT long-name entries. The macOS mount shows the names correctly and fsck is clean.
- The store remembers `last_build_id`: deleting all runs does not make the next boot re-run the tests.
- New `STATUS.TXT` (RAM, rendered at boot): firmware, chip, flash/JEDEC, results area (INVALID if so), what this boot did, and the save result with the reason (`store_last_error()`), verdict and stored runs. Console `s`.
- Host test `test_delete_via_disk`: deletes RUN0067 through MSC write10 (read-modify-write of the RUNS sector), then all of RUNS. Checks the deferral, the effect after remount, and that last_build_id is kept. All host tests pass.

**Hardware verification (WeAct RP2350B, ARM, files read from /Volumes/PICOBENCH)**
- RUN0001 was saved. The root REPORT.TXT is byte-identical to RUNS/RUN0001/REPORT.TXT. RESULTS.CSV has 61 rows, all with 27 columns. ERRORS.TXT says "No errors."
- STATUS.TXT after a replug: "no tests - this firmware build was already tested", verdict PASS, 1 stored run. The run-once logic works.
- Flash JEDEC `20 40 18`: manufacturer 0x20 (XMC XM25QH128 class), not Winbond as WeAct's docs say. Phase 3 should decode vendors.
- Counter vs timer: -33 ppm with the 50 ms window (was -120/-153 with 10 ms). clk_peri is 48 MHz at every operating point (normalisation works).
- macOS wrote `._REPORT.TXT` (AppleDouble) into the RAM overlay, as designed.
- `cpu.loop_smoke` max 348 again only in the first test after boot (XIP-miss hypothesis stands).
- Fixed after the check: STATUS.TXT did not show the `-dirty` flag (REPORT.TXT did).

**Next step**
- Phase 3: inventory + HARDWARE.TXT.

---

## 2026-09-19 — Phase 2: flash results store + USB report disk

**Done**
- Commit `6d16f0e`: phase 1 (verified on WeAct RP2350B ARM, including crash recovery).
- `src/storage/flash_layout.[ch]`: flash size from the JEDEC capacity byte (fallback 2 MB). The results area and 64 KB scratch sit at the end of flash; the firmware image is capped at 1 MB in the linker (confirmed in the .map: FLASH LENGTH 0x100000).
- `src/storage/results_store.[ch]`: A/B directory sectors (magic, version, CRC32, COMMITTED word programmed last in its own page) plus a ring of run blobs. Up to 16 runs. The write path is: dry-run sizing, placement, intermediate commit that drops overwritten runs, erase/program with watchdog keepalive, CRC read-back, then directory commit.
- Writers: REPORT.TXT (now with the run number), RESULTS.CSV (27 fixed columns, verified per record type by script), ERRORS.TXT.
- `src/usb/`: TinyUSB composite (CDC console + MSC) with its own descriptors (pid.codes 0x1209:0x0001). The read-only virtual FAT16 (16 MB, 512 B clusters) is generated on the fly, with file data served straight from XIP. README.TXT explains the files and the LED codes.
- `src/core/button.[ch]`: BOOTSEL via the QSPI CS trick (from RAM, IRQs off), and KEY GPIO23 on RP2350B (polarity-agnostic). Holding either for 1 s → `guard_request_rerun()` (scratch magic + watchdog reboot).
- `main.c`: new boot flow. The test runs only for a new build ID, a re-run request, or a crash resume. Saving is a watchdog stage of its own.
- Boot re-applies the default operating point, so clk_peri is consistent (48 MHz) at every point.
- `core.timing_counter`: window 10 → 50 ms (edge error /5).
- Host tests extended (`worklog/tools/host_tests/`): SDK stubs, NOR-flash simulation, `test_store.c`.
  - 60 runs with wrap and eviction: all content byte-exact.
  - 7807 power-loss injections (every erase/program operation in 12 store states): after "reboot" every listed run is intact, and the latest run is the old one or the new one.
  - The 16 MB image passes `fsck_msdos -n`, mounts read-only on macOS, and the files are identical.

**Findings**
- `sscanf` also pulls in newlib FILE code (`__retarget_lock_*`) and doesn't link with pqt-gcc; it is replaced by a manual parser (CLAUDE.md updated).
- RAM use: RP2040 ~72 KB .bss, RP2350 ~145 KB (arena + sample buffers), which leaves room for the SRAM tests.

**Hardware verification to do**
1. Flash the normal UF2: tests run (~1-2 s blinking), then the LED is solid briefly while saving, then PASS blinks. A `PICOBENCH` disk appears with README.TXT, REPORT.TXT, RESULTS.CSV, ERRORS.TXT and RUNS/RUN0001 (Windows and macOS).
2. Unplug and replug: no tests (LED shows the verdict immediately), the same disk appears, still RUN0001 only.
3. Hold BOOTSEL (or KEY on WeAct) for 1 s: reboot, tests, RUN0002 appears.
4. Crashtest UF2: the crash is visible in REPORT.TXT and ERRORS.TXT on the disk.
5. Serial console: `l` lists runs, `d` prints the report.

**Next step**
- Phase 3: inventory + HARDWARE.TXT (chip, bootrom, flash IDs/SFDP, QMI/SSI, PSRAM detection).

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
