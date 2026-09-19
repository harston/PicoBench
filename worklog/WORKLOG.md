# PicoBench work log

Newest entry first. Each entry: what was done, decisions, hardware results, open issues, next step.

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
