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
- `PICO_BOARD=picobench` (`boards/picobench.h`) is used for all three platforms. It sets
  only common denominators: 2 MB flash (bounds the image), LED GPIO25, W25Q080 boot
  stage 2 for RP2040.
- RP2350 builds target the **B package** (`PICO_RP2350A` left at the SDK default 0 → 48
  GPIO). Code must check the real package at runtime before touching GPIO 30–47.
- The SDK finds the Homebrew picotool 2.3.1 via its CMake config; nothing is downloaded.
- Build ID (`PICOBENCH_BUILD_ID` in the generated `picobench_version.h`) changes with every
  configure. `build.sh` always reconfigures, so each build gets a new ID.

`build.sh` must **back up existing UF2s in `dist/` before overwriting** — they are what
gets flashed to hardware.

- `PICOBENCH_VARIANT=crashtest ./build.sh` builds `dist/*_crashtest.uf2` with the
  `debug.crash` test (id 990), which hangs on purpose to exercise watchdog recovery.
- CMake option `PICOBENCH_DEBUG_CDC` (default ON until the USB disk exists): after the
  run, the text report is printed on a USB CDC serial port (`screen /dev/cu.usbmodem*`).
  Keys: `d` dump again, `r` re-run, `b` reboot to BOOTSEL.
- **Toolchain caveat:** newlib FILE functions (`fwrite`, `fflush`, `fputs` on streams)
  do not link with pqt-gcc (its newlib expects retargetable locks). Use `printf`
  (wrapped by pico_printf), `stdio_put_string()` and `stdio_flush()`.
- Host unit tests for platform-independent code (stats, CRC):
  `worklog/tools/host_tests/run.sh`. Run them after changing `src/core/stats.c` or
  `src/results/crc32.c`.

## Boot behaviour

- The full test runs **once** after a new firmware is flashed (its build ID differs from
  the last committed run), or when the button is held at power-up (BOOTSEL; KEY GPIO23
  on WeAct).
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
- **MSC is read-only** (`tud_msc_is_writable_cb` returns false).
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
- `src/detect/` — chip, board profile, flash ID, PSRAM detection, bootrom.
- `src/tests/` — one file per subsystem; each test registers
  `{id, name, subsystem, chip_mask, risk_level, fn}` in the static test table.
- `src/results/` — tests append binary TLV records to a RAM arena; writers stream
  REPORT.TXT, RESULTS.CSV, RESULTS.JSON, HARDWARE.TXT, ERRORS.TXT and RAW.BIN
  straight to flash through a 4 KB buffer (no whole file in RAM — RP2040 has 264 KB).
- `src/storage/` — results store at the end of flash: run history (RUN0001…), A/B slots.
- `src/usb/` — TinyUSB MSC serving a virtual FAT volume whose file sectors map directly
  onto XIP flash.

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
