# CLAUDE.md

Working notes for **PicoBench — Pico Hardware Characterization & Benchmark**.
Read this before changing anything. The full implementation plan lives in
`worklog/PLAN.md`; current progress lives in `worklog/WORKLOG.md`.

## What this is

A self-contained firmware (UF2) for RP2040 and RP2350A/B boards that, with **no
external equipment or wiring**, runs a full hardware characterization and benchmark
suite, stores the results in a dedicated power-loss-safe flash area, signals its state
on the on-board LED, and when finished exposes the reports as a **read-only USB mass
storage disk** (`PICOBENCH`).

The data feeds the Atari cartridge projects next door (`../PicoA10400`,
`../Pico2A10400`). What matters most there is **worst-case latency** (max, p99.9) of
the "cartridge hot path", not averages.

## Supported boards

| Board | Chip | Flash | LED | Button | PSRAM |
|---|---|---|---|---|---|
| WeAct RP2350B Core (primary) | RP2350B (48 GPIO) | 16 MB W25Q128 (external, the chip has none) | GPIO25 | KEY GPIO23 (+BOOTSEL) | optional footprint, CS = GPIO0 |
| Raspberry Pi Pico 2 | RP2350A (30 GPIO) | 4 MB | GPIO25 | BOOTSEL | none |
| Raspberry Pi Pico / "purple" clone | RP2040 | 2–16 MB | GPIO25 | BOOTSEL | none |

Pico W / Pico 2 W are out of scope (their LED sits behind the CYW43 radio chip).
The board is **detected at runtime**, never assumed from the build: chip ID, package
(RP2350A vs B), flash JEDEC/SFDP size, PSRAM probe on every XIP_CS1-capable pin
(0, 8, 19, 47 — trying GPIO0 first).

## Build

Plain Pico SDK **2.3.0** + CMake + Ninja (not Arduino — the Arduino runtime disturbs
measurements).

```sh
brew install cmake ninja                       # one-time
git clone -b 2.3.0 --recurse-submodules https://github.com/raspberrypi/pico-sdk ~/pico/pico-sdk
export PICO_SDK_PATH=~/pico/pico-sdk
./build.sh                                     # -> dist/picobench_{rp2040,rp2350_arm,rp2350_riscv}.uf2
```

Toolchains: the ARM and RISC-V gcc bundled with the Arduino core can be reused via
`PICO_TOOLCHAIN_PATH` (`~/Library/Arduino15/packages/rp2040/tools/pqt-gcc{,-riscv}/4.1.0-1aec55e`).
After a build, check with `picotool info -a dist/*.uf2`.

Build facts (verified in phase 0):
- `PICO_BOARD=picobench` (`boards/picobench.h`) is used for all three platforms: LED
  GPIO25, W25Q080 boot stage 2 for RP2040, and `PICO_FLASH_SIZE_BYTES` = **16 MB**.
- **Two different "flash sizes" - never mix them up:**
  - C define `PICO_FLASH_SIZE_BYTES` (board header) = the LARGEST supported chip (16 MB).
    The SDK uses it at runtime: `flash_range_erase/program` `hard_assert` against it
    (even in release builds), and on RP2350 it sets the FLASH_DEVINFO CS0 size. When it was
    2 MB, every write to the WeAct results area (12 MB) panicked and the watchdog reset
    the chip, so no report was ever saved.
  - CMake `PICO_FLASH_SIZE_BYTES` (CMakeLists.txt) = 1 MB = linker FLASH region, i.e.
    the firmware image limit.
- RP2350 builds target the **B package** (`PICO_RP2350A` left at the SDK default 0 → 48
  GPIO). Code must check the real package at runtime before touching GPIO 30–47.
- The SDK finds the Homebrew picotool 2.3.1 via its CMake config; nothing is downloaded.
- Build ID (`PICOBENCH_BUILD_ID` in the generated `picobench_version.h`) changes with every
  configure. `build.sh` always reconfigures, so each build gets a new ID.

`build.sh` must **back up existing UF2s in `dist/` before overwriting** — they are what
gets flashed to hardware.

- `PICOBENCH_VARIANT=crashtest ./build.sh` builds `dist/*_crashtest.uf2` with the
  `debug.crash` test (id 990), which hangs on purpose to exercise watchdog recovery.
- After the run the board enumerates as a composite USB device (pid.codes test VID/PID
  0x1209:0x0001): read-only disk `PICOBENCH` + CDC console (`screen /dev/cu.usbmodem*`,
  keys: `d` REPORT.TXT, `e` ERRORS.TXT, `l` list runs, `r` re-run, `b` BOOTSEL).
- **Toolchain caveat:** newlib FILE functions (`fwrite`, `fflush`, `fputs` on streams,
  and `sscanf`, which pulls in the same code) do not link with pqt-gcc (its newlib
  expects retargetable locks). Use `snprintf`/`vsnprintf` (wrapped by pico_printf) and
  parse by hand.
- Host tests: `worklog/tools/host_tests/run.sh` covers stats and CRC, and the results
  store with simulated NOR flash and **power loss injected at every erase/program
  operation**. It also generates the 16 MB FAT16 image, checks it with `fsck_msdos`, and
  mounts it read-only on macOS to compare the files. **Run it after any change in
  `src/core/stats.c`, `src/results/crc32.c`, `src/storage/` or `src/usb/msc_disk.c`.**

## Flash layout and results store (phase 2)

- Firmware image is capped at 1 MB (CMake `PICO_FLASH_SIZE_BYTES` = linker FLASH length).
  The flash size is detected at runtime from the JEDEC capacity byte (fallback 2 MB).
- From the end of flash: results area (512 KB / 1 MB / 2 MB / 4 MB for 2 / 4 / 8 / 16 MB
  flash), and 64 KB of flash-write scratch just before it. See `src/storage/flash_layout.h`.
- Results area = directory slot A, directory slot B, then a ring of run blobs. A
  directory is valid only with magic + version + CRC32 + a COMMITTED word programmed
  last in its own page; the higher sequence number wins. Runs that will be overwritten
  are dropped in an intermediate directory commit *before* their sectors are erased.
  Up to 16 runs are kept.
- Stored files per run: REPORT.TXT, HARDWARE.TXT, RESULTS.CSV, ERRORS.TXT
  (RESULTS.JSON and RAW.BIN come in a later phase). Writers are called twice (size, then write) and must be
  deterministic. They must not print anything that changes between the passes (e.g. the
  directory sequence number).
- The virtual disk (`src/usb/msc_disk.c`) is generated on the fly: 16 MB FAT16, 512-byte
  clusters, files read straight from XIP. `README.TXT` is a firmware constant and
  `STATUS.TXT` is rendered at boot (what this boot did, whether the save worked and why
  not). Hidden `.Trashes` (a *file*, so Finder deletes immediately), `.metadata_never_index`
  and `.fseventsd/no_log` keep macOS from filling the RAM overlay. They use VFAT long names.
- Host writes are kept in a RAM overlay (the reused `g_bench_samples` buffer: 32 KB on
  RP2040, 64 KB on RP2350). When a RUNxxxx directory entry disappears, that run is deleted
  from the store 300 ms after the host goes quiet. `last_build_id` in the store directory
  survives deletions, so deleting results never re-triggers the tests.
- Saving is its own watchdog stage (stage number = number of test stages). A hang
  while saving is retried once, then skipped.

## Boot behaviour

- The full test runs **once** after a new firmware is flashed (its build ID differs from
  the latest stored run). To run again: hold BOOTSEL (or KEY GPIO23 on RP2350B) for 1 s
  while the disk is shown (the LED goes solid while held), or press `r` on the console.
  BOOTSEL held at *power-up* enters the ROM bootloader instead, which is not our code.
- The KEY on GPIO23 is used only on RP2350B (on Pico / Pico 2 that pin is the SMPS mode
  pin). It is an input with pull-up, active low. If it reads low for seconds after boot
  it is not a button on that board and is ignored.
- **A button held at boot must be released before it counts as a press.** The press that
  requested the re-run is still held while the board reboots; without this rule one long
  press produced two runs (observed on the WeAct). The action happens **on release**:
  1-5 s = run the tests again, 5 s or more = erase every stored run first (the LED
  switches from solid to blinking when the erase threshold is passed).

### Erasing the stored runs

`next_run_no` lives in the store directory in flash (`store_dir_t`), not on the USB
disk, so deleting folders never resets numbering - run numbers identify reports and are
never reused. Three ways to wipe the store and restart at RUN0001, all calling
`store_erase_all()` (erases both directory sectors, clears `last_build_id`, so the next
boot tests again):
- console `Z`, confirmed by pressing `Z` again within 10 s,
- deleting `README.TXT` on the disk (no terminal needed),
- holding the button for 5 s and releasing.
- Every other boot skips the tests and only mounts the report disk.
- Flashing a new UF2 does not erase the results area, so run history survives upgrades.

## LED protocol (hardware PWM, zero CPU cost during measurements)

| Pattern | Meaning |
|---|---|
| fast blink ~4 Hz | tests running — do not unplug |
| solid on | writing/verifying flash — do not unplug |
| 1 short blink every ~2 s | finished, PASS — safe to unplug |
| 2 short blinks every ~2 s | finished with errors, report saved — safe to unplug |

## Hard rules

- **All code comments, docs, reports and commit messages in English**, whatever language
  the conversation is in.
- **USB is off during measurements.** MSC starts only after the report is committed.
  The one exception is the dedicated "USB interrupt impact" test.
- **No overclocking.** Operating points: RP2350 48/96/120/133/150 MHz, RP2040
  48/96/120/125/133 MHz. Always read back the real clock configuration.
- **Every risky test** (QMI/PSRAM timing, clock changes, flash writes) is wrapped by the
  watchdog, with its stage ID written to watchdog scratch first. On RP2350, scratch 4–7
  belong to the bootrom — use 0–3 only. A reset mid-test becomes `TEST_CRASHED` and
  the run continues; it must never boot-loop.
- **Never write flash outside the results area and the flash-write scratch area.** The
  results area is reserved in the linker script so the binary can never overlap it.
- **Results store is A/B with CRC32 and a final COMMITTED word**: write to the inactive
  slot, read back and verify, then commit. Never touch the committed slot while writing.
- **USB disk writes go to a RAM overlay.** The only change persisted to flash is
  deleting `RUNS\RUNxxxx` (or `RUNS`), which becomes `store_delete_runs()` (A/B commit).
  Never let the host write into the results area directly.
- **Store raw cycles; derive ns** from the measured clk_sys. Always report
  min/p50/p95/p99/p99.9/max plus iteration counts and buffer sizes, never a mean alone.
  A PASS verdict must never hide a large max latency.
- **Internal vs electrical timing:** GPIO results are internal (CPU/PIO→pad register)
  only. The report must say so explicitly. Rise time, PCB propagation and the Atari
  threshold cannot be measured from the chip.
- **Don't assume hardware exists**: missing features (PSRAM, SHA, TRNG on RP2040, etc.)
  produce `N/A` / `NOT PRESENT` records rather than being silently skipped.
- Every report must be comparable before/after (e.g. PSRAM soldering): firmware version
  and build ID, test set, clocks, temperature, buffer sizes, iterations, QMI config.

## Architecture (summary — details in worklog/PLAN.md)

- `src/main.c` — boot flow: retention check → run/skip decision → sequencer → report → MSC.
- `src/core/` — timing (DWT CYCCNT on M33, `mcycle` on Hazard3, SysTick on M0+), stats,
  clock operating points, LED, watchdog guard, retention.
- `src/detect/` — inventory tests (flag `TF_INVENTORY`, rendered into HARDWARE.TXT):
  chip/ROM/OTP info, firmware/SDK/compiler, unique IDs, inferred board profile, memory
  map, reset reason, temperature/VREG/VSYS, clock sources, flash identity (JEDEC +
  vendor table, SFDP density, status registers, unique ID), flash interface (SSI / QMI
  decode), PSRAM detection (RP2350: probes XIP_CS1 GPIOs 0/8/19/47, raw MFID/KGD/EID,
  SDK default init, first/last word sanity), USB config.
- `g_psram` (`src/detect/psram_info.h`) holds the PSRAM detection result for later
  stages. It is RAM only and is not re-created after a crash-resume.
- `src/tests/` — one file per subsystem; each test registers
  `{id, name, subsystem, chip_mask, risk_level, fn}` in the static test table.
- `src/results/` — tests append binary TLV records to a RAM arena; writers stream
  REPORT.TXT, RESULTS.CSV, RESULTS.JSON, HARDWARE.TXT, ERRORS.TXT and RAW.BIN
  straight to flash through a 4 KB buffer (no whole file in RAM — RP2040 has 264 KB).
- `src/storage/` — results store at the end of flash: run history (RUN0001…), A/B slots.
- `src/usb/` — TinyUSB MSC serving a virtual FAT volume whose file sectors map directly
  onto XIP flash.

### QMI is shared - flash and PSRAM are not independent

- `rom_flash_select_xip_read_mode()` configures **both chip selects**, so the XIP mode
  sweep leaves the PSRAM window set up as if it were flash. Any test that reconfigures
  QMI must call `psram_reapply()` afterwards, and every PSRAM test calls it on entry
  rather than trusting the state left behind (this produced 10 M bogus "PSRAM errors").
- Measured on the WeAct with an 8 MB APS6404: a PSRAM DMA stream stretches a **flash
  cold cache miss from 171 to 436 cycles (+154 %)** and collapses flash streaming reads
  by ~30x, because one PSRAM transaction can hold the controller for up to max_select
  (18 x 64 = 1152 clk_sys cycles ~ 7.7 us at 150 MHz). The reverse direction costs PSRAM
  only ~21 %. For the cartridge: keep critical code and tables in SRAM and keep PSRAM
  traffic out of time-critical windows.

### Board facts learned on hardware

- WeAct RP2350B: RP2350-A3 silicon, boot ROM 4. Flash JEDEC `20 40 18` (XMC XM25QH128
  class), 16 MB, not Winbond as WeAct's docs say. XIP quad 1-4-4 (0xEB), SCK 50 MHz.
- Pico 2 clone: **RP2350-A2** silicon, boot ROM 2, Boya BY25Q128 16 MB (`68 40 18`), and
  the boot ROM leaves XIP in **dual** mode (0xBB, 1-2-2, QE bit not set) - about half the
  flash read bandwidth of the WeAct. Keep both boards in benchmark comparisons.
- **The RP2350 build targets the B package**, so anything indexed by chip variant must be
  resolved at runtime: the temperature sensor is ADC channel 8 on B but 4 on A (a
  compile-time constant gave "411 degC" on the clone), and GPIO 30-47 do not exist on A.

### Adding a test

1. Write `test_result_t test_xxx(test_ctx_t *ctx)` in the matching `src/tests/*.c` and
   declare it in `src/tests/tests.h`.
2. Add a row to `g_tests[]` in `src/tests/registry.c`: a **new, never reused ID** in
   the subsystem's range, `chip_mask` (`CHIP_RP2040`, `CHIP_RP2350A/B`), and flags
   (`TF_SWEEP` = also run at every operating point, `TF_RISKY`).
3. Report only through the record API (`rec_stats`, `rec_value`, `rec_text`,
   `rec_error`, `rec_hist`). Store cycles; the writers derive ns. Use the shared
   `g_bench_samples[]` (exact percentiles) or `stats_stream_*` with
   `g_bench_stream_bins` (millions of samples). Never format text inside a measurement.
4. Measurement loops: IRQs off (`save_and_disable_interrupts`), code under test in
   SRAM (`__not_in_flash_func`) unless XIP is what's being measured,
   `timing_now()`/`timing_net()` around the work.

### Measurement pitfalls (all three cost a wrong result on hardware in phase 4)

At `-O3` the compiler will happily destroy a benchmark. After writing a kernel,
**check the disassembly** (`build/<platform>/picobench.dis`) and count the instructions:

- **A pure loop is hoisted out of the timed region.** A read-only loop called through a
  known function pointer gets inlined, proved loop-invariant and moved out - the test
  then reports *0 cycles*. Call the kernel through a `volatile` function pointer and
  pass its arguments through `__asm volatile("" : "+r"(x))`.
- **Repeated operations are merged algebraically.** Eight `a = a * b` become one
  multiply by `b^8`, eight adds become `a + 8b`. Put a register fence
  (`__asm volatile("" : "+r"(a))`, no instruction emitted) between every operation.
- **A constant divisor is not a division.** `a / 0x9E3779B9` becomes multiply+shift.
  Fence the divisor so its value is unknown to the compiler.
- **Unaligned access must use a packed type.** A plain `uint32_t *` at an odd address
  lets the compiler emit `LDRD`/`LDM`, which fault even on Cortex-M33 (this hung
  `sram.throughput` on every operating point). Only M33 may do unaligned word loads at
  all; M0+ and Hazard3 record why instead.
- Unrolling matters for anything measured per access: with a tight loop the loop
  overhead hid the difference between access strides entirely.
- **`__not_in_flash_func` does not stop inlining.** A static helper marked that way was
  inlined into its flash-resident caller, so the SRAM copy was never executed - fatal
  for code that runs while flash is unreadable (XIP mode switching). Use
  `__no_inline_not_in_flash_func` there and check the symbol's address in the `.dis`.
5. A test running longer than ~4 s must call `guard_keepalive()` (watchdog is 5 s).
6. Returning `TR_PASS` with `ctx->errors > 0` is turned into `TR_FAIL` automatically.

### Crash recovery (verified design, phase 1)

- The stage number is written to watchdog scratch before each stage. The results arena
  lives in `.uninitialized_data`, so it survives a watchdog reset. A header with magic,
  build ID, nonce and CRC32 decides whether the arena can be resumed.
- After a reset inside a stage: resume at that stage, record `REC_CRASH` +
  `TEST_CRASHED`, and continue with the next stage. If the arena is invalid, restart at
  stage 0 and skip the crashed stage. After `GUARD_MAX_CRASHES` (8) crashes, stop testing
  and report only.
- RP2040 cycle timing uses the 24-bit SysTick: a single `timing_now()` interval must
  stay below 2^24 cycles (~126 ms at 133 MHz).

## SRAM notes

- RP2350: 520 KB = banks 0–3 (word-striped, 0x20000000–0x2003FFFF), banks 4–7
  (striped, 0x20040000–0x2007FFFF), banks 8 and 9 (4 KB each, scratch X/Y). A single bank
  is addressed with a 16-byte stride.
- RP2040: 264 KB = 4 striped banks + 2 × 4 KB. There's a non-striped alias at 0x21000000.
- Regions the firmware itself uses are tested chunk-by-chunk with save/test/restore,
  with interrupts off, from code running in another bank.

## Worklog

- Record every working session in `worklog/WORKLOG.md` (date, what was done, hardware
  results, open issues, next step), newest entry at the top.
- Put all helper scripts, analyses and host tools in `worklog/` (host Python tools in
  `worklog/tools/`).
