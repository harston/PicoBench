# PicoBench

PicoBench is a self-contained firmware that characterizes and benchmarks Raspberry Pi
RP2040 and RP2350 boards (CPU, clocks, SRAM, flash/XIP, PSRAM, DMA, PIO, GPIO,
multicore, interrupts) with no external equipment or wiring. Flash it, wait for the
LED, and read the reports from the USB disk the board turns into.

It focuses on **worst-case latency** (p99.9 and max), not averages, because it was
written to size the hot path of Atari cartridge emulators running on these chips.

## Supported boards

| Board | Chip | UF2 |
|---|---|---|
| Raspberry Pi Pico and clones | RP2040 | `dist/picobench_rp2040.uf2` |
| Raspberry Pi Pico 2 and clones | RP2350A | `dist/picobench_rp2350_arm.uf2` or `_riscv.uf2` |
| WeAct RP2350B Core (with or without PSRAM) | RP2350B | `dist/picobench_rp2350_arm.uf2` or `_riscv.uf2` |

The `_arm` image runs on the Cortex-M33 cores, the `_riscv` image on the Hazard3 cores.
The `_overclock` images next to them are a separate, short run that goes outside the
chips' specification; see [Overclocking variant](#overclocking-variant) before using
them. The board, flash size and PSRAM are detected at run time. Pico W and Pico 2 W are not
supported (their LED is behind the radio chip).

## How it works

1. **Flash.** Hold BOOTSEL while plugging the board in and copy the UF2 to the
   `RPI-RP2` / `RP2350` drive.
2. **Test.** The full suite runs once, automatically, at every operating point
   (RP2040: 48/96/120/125/133/150/200 MHz, RP2350: 48/96/120/133/150 MHz). All are within
   spec: the RP2040 has been rated for 200 MHz at a 1.15 V core supply since Pico SDK 2.1,
   and PicoBench raises the voltage for 150 and 200 MHz the way the SDK does. USB is off
   while the suite runs so it cannot disturb the measurements. The LED blinks fast.
3. **Save.** The results are written to a reserved area at the end of the flash
   (A/B slots, CRC32, power-loss safe; the last 16 runs are kept). The LED is solid.
4. **Read.** The board appears as a USB disk named `PICOBENCH` plus a serial console.
   The LED now shows the result:

| LED | Meaning |
|---|---|
| fast blink (~4 Hz) | tests running - do not unplug |
| solid on | writing the report to flash - do not unplug |
| 1 short blink every ~2 s | finished, PASS - safe to unplug |
| 2 short blinks every ~2 s | finished with errors, report saved - safe to unplug |

Later boots skip the tests and only show the disk. To run again, hold BOOTSEL (or KEY
on the WeAct RP2350B) for 1-5 s while the disk is shown and release it; holding it for
5 s or more erases all stored runs first. A test that hangs is reset by the watchdog,
recorded as `TEST_CRASHED`, and the run continues with the next test.

The serial console (`screen /dev/cu.usbmodem*` on macOS) accepts single keys:
`d` print REPORT.TXT, `e` ERRORS.TXT, `s` status, `l` list runs, `r` run again,
`b` reboot to BOOTSEL, `Z` (twice) erase all runs, `h` help.

## Reading the results

| File | Contents |
|---|---|
| `REPORT.TXT` | human-readable report of the latest run - start here |
| `HARDWARE.TXT` | what was detected: chip and silicon revision, flash chip, PSRAM, clocks, memory map, free pins |
| `RESULTS.CSV` | every measurement, one row per record, for spreadsheets and scripts |
| `ERRORS.TXT` | crashes, failed tests and memory errors (empty on a clean run) |
| `STATUS.TXT` | what the current boot did: were tests run, was the report saved |
| `RUNS\RUNxxxx\` | the same files for every stored run |

The disk is writable, but only deletions reach the flash: deleting a `RUNS\RUNxxxx`
folder (or `RUNS`) deletes those runs, and deleting `README.TXT` erases every stored run
and restarts numbering at RUN0001. Anything else you write or change is kept in RAM and
disappears when the board restarts.

`REPORT.TXT` starts with the firmware build, chip, inferred board and the clocks
measured at each operating point, then lists every test:

```
  [100] core.timing_counter @ 125 MHz
    counter                  SysTick (24-bit)
    read_overhead            2 cycles
    back_to_back_read        n=4096   min 2  p50 2  p95 2  p99 2  p99.9 2  max 53  mean 2.012 cycles
                             ns     min 16.000  p50 16.000  p95 16.000  p99 16.000  p99.9 16.000  max 424.000
    => PASS  (53.451 ms, 0 error(s))
```

- `[100] core.timing_counter @ 125 MHz` - test ID, test name, and the clock it ran at.
  Tests marked as sweeps appear once per operating point.
- Latency lines give the sample count `n` and the distribution in **CPU cycles**:
  min, median (p50), p95, p99, p99.9 and max. The `ns` line below is derived from the
  measured clock. Judge timing by **p99.9 and max** - above, one read in 4096 took 53
  cycles instead of 2, and that is the one that matters for a real-time loop.
- `=> PASS / FAIL / N/A / SKIPPED / TEST_CRASHED` is the test result. **PASS means the
  test ran and found no errors; it does not mean the latency is good enough for your
  application.** `N/A` means the hardware is not there (for example no PSRAM, or a
  feature the chip lacks) and is not a failure.

The end of the report has a verdict per subsystem (`PASS`, `FAIL`, `N/A`,
`NOT PRESENT` for missing PSRAM, `NOT RUN`) and one `OVERALL: PASS` or `FAIL` line.

All timings are **internal** (CPU, bus, PIO to the pad register). Electrical behaviour
such as rise time, PCB propagation or input thresholds cannot be measured from inside
the chip and is not reported.

To compare boards or before/after runs (e.g. after soldering PSRAM), copy the run
folders off the disk and run:

```sh
worklog/tools/compare_runs.py WeAct=path/to/RUN0003 Pico2=path/to/RUN0001 > comparison.md
```

## Results so far

Three boards measured: a purple Raspberry Pi Pico clone (RP2040, 16 MB Boya flash), a
black Pico 2 clone (RP2350A, 16 MB Boya flash) and a WeAct RP2350B Core (16 MB XMC flash
and 8 MB PSRAM). Figures at each board's default clock (125 / 150 / 150 MHz) unless the
row says otherwise. On the RP2350, Cortex-M33 / Hazard3 figures are given where they
differ.

| | RP2040 | RP2350A | RP2350B |
|---|---|---|---|
| Cartridge hot path in SRAM (p50 / max) | 264 / 320 ns | 133 / 153 ns (H3 120 / 127) | 133 / 153 ns (H3 120 / 127) |
| Flash read mode the board boots in | quad, no command byte | dual, command byte | quad, command byte |
| Flash cache miss, boot mode | 0.42 us | 1.15 us | 0.75 us |
| Flash cache miss, best setting tested | 0.27 us (200 MHz, SCK 100 MHz) | 0.26 us (133 MHz, quad continuous, SCK 133 MHz) | 0.26 us (same) |
| Uncached flash read, boot mode / best tested | 9.6 / 15.3 MB/s | 7.2 / 24.9 MB/s | 11.0 / 24.9 MB/s |
| SRAM dependent load | 3.52 cycles | 2.26 cycles (H3 3.01) | 2.26 cycles (H3 3.01) |
| DMA copy SRAM to SRAM | 496 MB/s | 597 MB/s | 597 MB/s |
| SRAM read while a DMA copy runs in the same banks | +36 % | +4 % | +4 % |
| PSRAM uncached read / worst flash miss during PSRAM DMA | - | - | 0.37 us / 1.85 us (read 0.25 us at 133 MHz, divisor 1) |
| Highest clock in the overclocking run (1.30 V, outside spec) | 420 MHz confirmed, 432-450 passed | ~320 MHz at any voltage | 412 MHz (M33), 420 MHz (Hazard3) |

- **The two RP2350 boards are identical in clock cycles.** Their only difference is the
  flash read mode the boot ROM picks: dual on the black clone, whose flash has its Quad
  Enable bit clear, and quad on the WeAct.
- **A flash cache miss costs one SPI frame** (frame length x clock divider + ~5 cycles).
  Quad with continuous read at SCK 75 MHz brings both RP2350 boards to 0.41 us, the
  RP2040's level. The black clone gets there once QE is set (a volatile write is enough).
- **Every latency is the same number of cycles from 48 to 200 MHz**, so its time scales
  with 1/f. PSRAM is the exception: the SDK changes its divider at 120 MHz.
- **133 MHz is the best memory setting on the RP2350.** With divider 1 for both flash
  and PSRAM, a flash miss takes 0.26 us and a PSRAM read 0.25 us. The PSRAM is rated for
  133 MHz. The flash read correctly at SCK 133 MHz on both boards, but its rated maximum
  was not checked. The RP2040 runs 1.6x faster at its rated 200 MHz.
- **Overclocking** (separate firmware, outside spec) reaches 420-450 MHz on the RP2040
  and 412-420 MHz on the WeAct RP2350B. A flash miss improves only to 0.23 us there,
  because the flash clock stays at 133 MHz or less. The black RP2350A clone stops at ~320 MHz
  whatever the voltage, probably limited by its regulator circuit.

Full comparison - CPU, SRAM, every flash read mode and clock, PSRAM, the cartridge hot
path, peripherals and clock scaling:
[docs/board-comparison.md](docs/board-comparison.md), or with charts
[docs/board-comparison.html](docs/board-comparison.html).

## Overclocking variant

`PICOBENCH_VARIANT=overclock ./build.sh` builds `dist/picobench_<chip>_overclock.uf2`.
This short run finds how far each chip goes **outside its specification**:

- **Clock search.** At each core voltage from 1.10 V to 1.30 V it raises clk_sys in
  10 MHz steps until something fails. 1.30 V is the regulator's limit while it is
  locked, and PicoBench never unlocks it. At every step, for 250 ms:
  - both cores run a checked integer kernel;
  - core 0 also runs the ARM interpreter, SRAM and DMA patterns, a flash read-back and a
    PSRAM pattern;
  - the step records the interpreter speed, flash-miss and PSRAM latency, and chip
    temperature.
- **Memory clocks are limited.** During the clock search flash runs at 75 MHz or less
  and PSRAM at 133 MHz or less. At the highest clock found, a flash probe tries the
  fastest flash setting up to SCK 133 MHz, reading only.
- **SCK limits.** At the normal operating points this run also finds the fastest flash
  SCK up to 133 MHz and PSRAM SCK up to 150 MHz.
- **Report.** `oc.summary` in `REPORT.TXT` gives the highest confirmed clock per voltage.

Use it at your own risk. Its results are specific to one chip at one temperature over a
few seconds, so treat them as limits rather than clocks to run at. Higher voltage and
clock wear the chip faster. If a step hangs, the watchdog resets the board and the run
continues with the next voltage, and the core voltage returns to its default at every
boot. The UF2s are in `dist/` next to the normal ones, but only flash them if you accept
running your board outside its specification.

## ARM workload (optional)

Two tests (`cpu.thumb_interp`, `hotpath.arm_neighbour`) replay real ARM code of Atari
2600 Harmony cartridge games through a Thumb interpreter, to check whether these chips
can emulate such cartridges. That code belongs to the games' authors, so it is not in
this repository and the published UF2s don't include it. Without it `cpu.thumb_interp`
reports `N/A`; `hotpath.arm_neighbour` still runs, with a synthetic ARM load written for
PicoBench, and the game code only adds a comparison variant.
To run them, generate the workload from ROMs you own with
`worklog/tools/arm_workload/awl_tool.py`. It identifies the ROMs by CRC32, records them in
the Gopher2600 emulator and keeps only the code the tests need. See
[workload/README.md](workload/README.md).

## Building

Pico SDK 2.3.0, CMake and Ninja (plain SDK, no Arduino runtime):

```sh
git clone -b 2.3.0 --recurse-submodules https://github.com/raspberrypi/pico-sdk ~/pico/pico-sdk
export PICO_SDK_PATH=~/pico/pico-sdk
./build.sh      # -> dist/picobench_{rp2040,rp2350_arm,rp2350_riscv}.uf2
```

`build.sh` expects the ARM and RISC-V GCC toolchains; see the top of the script for the
variables that point to them. `PICOBENCH_VARIANT=overclock` builds the overclocking
variant described above. When a local ARM workload exists, it also builds
`dist/picobench_<chip>_local.uf2` with it; the plain images never contain it.
Host-side tests of the statistics, CRC, results store and virtual disk:
`worklog/tools/host_tests/run.sh`.

## License

[GPL-3.0-or-later](LICENSE). The firmware images also contain the Raspberry Pi Pico SDK,
TinyUSB and a printf implementation under their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
