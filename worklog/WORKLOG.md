# PicoBench work log

Newest entry first. Each entry: what was done, decisions, hardware results, open issues, next step.

---

## 2026-09-21 — Phase 6: PSRAM (written before the chip is fitted)

**Done** (all RP2350-only; without a PSRAM chip every stage reports N/A and the run is unaffected)
- `psram.integrity` (510, risky): full-chip passes for const 00/FF, address and LFSR, then 1 MB passes for const 55/AA, walking ones/zeros and inverted address, plus byte/halfword access and March C- over 1 MB. Two extra checks: an **address-wrap probe** (a marker at offset 0 versus writes at each power-of-two offset - catches a chip smaller than it claims) and the **first and last word of every 1 MB region**. Everything goes through the uncached alias so the XIP cache cannot hide a fault. Watchdog keepalive every 64 KB.
- `psram.bench` (511, swept): re-applies the SDK timing profile for the current clk_sys first (the PSRAM clock is clk_sys/divisor, so a swept test must retime) and reports the divisor, SCK, rxdelay and select times it got. Then read/write 32/8-bit at 4 KB and 64 KB, cached and uncached; single-access latency as hit / cold miss / uncached; and dependent-load latency over 16 KB (fits the cache) and 256 KB (does not).
- `psram.copy` (512): memcpy and DMA for SRAM->PSRAM, PSRAM->SRAM and PSRAM->PSRAM at 4 KB and 64 KB.
- `psram.timing_sweep` (513, risky): divisor 4..2 against rxdelay div-1/div/div+1, each applied with the SDK's own select/deselect maths, then verified by writing and reading back a 256 KB LFSR pattern. Records stable or the number of bad words, and always restores the default profile and proves it works.
- `qmi.conflict` (520): the cartridge-relevant one. Flash cold-miss latency and flash streaming read, alone and while DMA hammers PSRAM; then PSRAM streaming while DMA reads flash. Reports slowdowns, worst cases, and the DMA words actually moved.

**Notes**
- PSRAM is initialised by `psram.detect` in phase 3; these tests re-check `g_psram.present` because a crash-resume does not repeat the detection stage.
- The SDK's `psram_initialize_internal` is RAM-resident, so reconfiguring the PSRAM window is safe from flash-resident code (that is what the SDK's own boot path does).

**Hardware verification (WeAct RP2350B with an 8 MB APS6404 fitted by the user, RUN0016, 68/68 stages, 0 crashes)**
- Detection: `probe_gpio0` -> MFID 0d, KGD 5d, EID 53, 8 MB, AP Memory class, first/last word sanity OK.
- **`psram.integrity` reported 10.3 M errors - my bug, not the hardware.** `rom_flash_select_xip_read_mode()` configures *both* chip selects (it says so in the SDK header), so `flash.xip_mode_sweep`, which runs two stages earlier, left the PSRAM window configured as flash. Reads returned 0xcccccccc and the size probe wrongly concluded "64 KB".
  - Fixed: new `psram_reapply()` helper (src/detect/psram_detect.c); the mode sweep restores the PSRAM window and reports whether it succeeded; `psram.integrity` re-applies the profile on entry instead of trusting earlier stages; the size probe now distinguishes "mirrored address" from "chip not answering".
- Everything that re-initialised PSRAM itself produced clean results, which is the proof the soldering is good:
  - **timing sweep: 9 of 9 combinations stable**, zero bad words in 256 KB: 37.5 MHz 15.6 MB/s, 50 MHz 20.3 MB/s, 75 MHz 28.7 MB/s.
  - throughput at 75 MHz: read 27.3 MB/s, write 33.7 MB/s; cached read inside 16 KB 251 MB/s.
  - latency: hit 8 cycles, cold miss 124, uncached single read 59; **dependent load 53.1 cycles uncached vs 2.3 cached**.
  - copies: SRAM->PSRAM 20.6 (CPU) / 34.5 (DMA) MB/s, PSRAM->SRAM 15.6 / 29.1, PSRAM->PSRAM 6.6 / 8.0.
- **`qmi.conflict` - the headline result for the cartridge**: with a PSRAM DMA stream running, a flash cold miss goes from **171 to 436 cycles (+154 %)** and flash streaming reads collapse by ~30x; the reverse (flash DMA vs PSRAM reads) costs only 21 %. Cause: `max_select` = 18 means one PSRAM transaction can hold QMI for 1152 clk_sys cycles (~7.7 us at 150 MHz). Written up in CLAUDE.md.

**Next**
- Re-run with the fix: `psram.integrity` should pass cleanly. Then phase 7 (cartridge hot path), where this QMI finding directly shapes the variants worth measuring.

---

## 2026-09-20 — Phase 5: flash, XIP and cache

**Done**
- `src/tests/flash_xip.c`
  - `flash.read` (410, swept): cached warm 4 KB (fits the 16 KB cache) for 32/16/8-bit, cached streaming 64 KB (four times the cache), and the uncached alias for 4 KB and 64 KB, all with kB/s.
  - `flash.cache` (411): hit latency, **cold-miss latency with the cache invalidated and a fresh address for every sample** (min/p50/p95/p99/max plus the worst case), uncached single read, one read per cache line over 64 KB, a working-set sweep from 2 KB to 64 KB that shows where the 16 KB cache stops holding the data, and the cost of `xip_cache_invalidate_all()`.
  - `flash.code_exec` (412, swept): the same loop executed from SRAM, from flash warm, and from flash with the cache invalidated before every call, with the overhead in percent and the worst case in cycles. This is the number the cartridge design depends on.
  - `flash.copy_to_sram` (413): memcpy 1-64 KB, cached and uncached.
  - Sample counts scale with the block size (~1 MB of traffic per measurement) instead of a fixed 256, so a 64 KB block does not read 16 MB through a 25 MB/s interface.
- `src/tests/flash_write.c` (420, risky): erase a sector, verify erased, program a page, program the rest of the sector, verify, erase four sectors, verify, program again - all inside the reserved 64 KB scratch area, with times from the 1 MHz timer. About six erase cycles per run against a 100k-cycle endurance rating.
- `src/tests/flash_mode.c` (430, risky, RP2350): sweeps read mode (03h serial, 0Bh fast serial, BBh dual, EBh quad) against clock dividers 4/3/2. For each combination it switches the mode from SRAM with interrupts off, verifies a 32 KB checksum through the uncached alias, measures only if the checksum matches, and always restores the original mode and timing register. **It never writes to the flash chip**, so the QE bit is untouched and a chip with QE clear simply reports the quad modes as unusable - which answers the clone question directly.
- 32 tests, 7 of them swept -> 60 stages.

**Bug caught before hardware (the .dis check earned its keep)**
- `try_mode`, `ram_checksum` and `ram_read_block` were marked `__not_in_flash_func`, but the compiler inlined all three into `test_flash_xip_mode_sweep`, which lives in flash (0x10005dc4). The mode switch would then have executed from flash while flash was unreadable. Fixed with `__no_inline_not_in_flash_func`; verified that `try_mode` now sits at 0x200184fc and makes no calls into flash (only ROM function pointers and SRAM helpers). Written up in CLAUDE.md.

**Hardware verification (WeAct RP2350B RUN0015 and purple RP2040 clone RUN0002; 59/59 stages, 0 crashes, 0 errors each; the RP2350A clone directory was empty)**

| | WeAct RP2350B @150 MHz (quad, SCK 50 MHz) | purple RP2040 @125 MHz (SCK 62.5 MHz) |
|---|---|---|
| cache hit (single read) | 8.0 cycles | 8.0 cycles |
| **cold miss, single read** | **175 cycles** | **215 cycles** |
| miss per cache line, streaming | 78.8 cycles | 53.8 cycles |
| XIP streaming read (64 KB) | 11.3 MB/s | 9.3 MB/s |
| uncached read | 11.0 MB/s | 9.6 MB/s |
| cached read inside 16 KB | 238.6 MB/s | 142.0 MB/s |
| code from flash, warm | same as SRAM (0 %) | same as SRAM (-1 %) |
| code from flash, cold cache | +28 % | +19 % |
| erase 4 KB sector | 2.2 ms | **28.9 ms** |
| program 256 B page | 511 us | 369 us |
| verify errors | 0 | 0 |

- **The 16 KB cache boundary is sharply visible**: 2.5 cycles per word for working sets of 2-16 KB, then 45.6 cycles at 32 KB (RP2040: 3.5 -> 47.0). Anything that must be fast has to fit in 16 KB or live in SRAM.
- The measured streaming rate matches the model from the earlier analysis: quad at 50 MHz needs ~36 SCK per 8-byte line = 4.5 SCK/byte -> 11.1 MB/s, measured 11.3.
- `xip_cache_invalidate_all()` costs 12657 cycles on RP2350 but only 1303 on RP2040.
- The purple clone's flash erases a sector in 28.9 ms, thirteen times slower than the WeAct's XMC part. Worth knowing before designing anything that writes flash at run time.
- **DMA contention, now with proof**: the reworked test moved 530170 words on RP2350 (**5 % CPU slowdown**, 458 MB/s of DMA traffic) and 1048576 words on RP2040 (**25 % slowdown**). The earlier "0 % / 7 %" came from a DMA configuration that barely generated traffic.

**Bug found in the run and fixed**
- The XIP mode sweep reported "1 cycles for 4 KB" for all twelve combinations: the pure read block was moved out from between the two cycle-counter reads. Same class as the phase 4 bugs; now called through a volatile function pointer with a register barrier (verified in the .dis). The checksums and the restore were fine - all 12 combinations verified correct on the WeAct, and the flash still read correctly afterwards.
- Result arena enlarged (RP2040 64 KB, RP2350 128 KB): phase 5 already used 81 % of the old RP2040 arena.

**RP2350A clone (RUN0004, 59/59 stages, 0 crashes, 0 errors) - the dual-mode question answered**
- **`EBh quad` is UNUSABLE on this board at every divider**: the checksum differs from the reference every time (0x08885630 / 0xbc430723 / 0x8f55e7d4 vs 0x3b430a63), exactly as expected for a Boya chip whose QE bit is clear. The test detected it by checksum, restored the original mode immediately and finished the run cleanly - the safety design works.
- Serial and dual modes are usable at every divider including 2 (SCK 75 MHz).

| dual-mode clone vs quad WeAct | WeAct | clone | ratio |
|---|---|---|---|
| cold cache miss | 175 cycles | **283 cycles** | 1.62x |
| per-line miss, streaming | 78.8 | **131 cycles** | 1.66x |
| XIP streaming read | 11.3 MB/s | **7.4 MB/s** | 1.53x |
| code from flash, cold | +28 % | **+51 %** | - |
| erase 4 KB sector | 2214 us | **870 us** | Boya is 2.5x faster |
| program 256 B page | 511 us | 779 us | - |

- The earlier model predicted 1.55x; measured 1.62x. The "slower" chip is markedly faster at erasing.
- DMA contention on the clone matches the WeAct exactly: 530374 words, 5 % slowdown.
- Still pending: the mode-sweep *timings* (the "1 cycle for 4 KB" bug is fixed but that build has not been run yet).

**Decision: PSRAM goes on the WeAct now**
- Baseline without PSRAM is complete for all three boards, detection already works (phase 3), so phase 6 can be tested as soon as it is written. Recommended part: APS6404L-3SQR (8 MB, KGD 0x5D); CS is GPIO0 on the WeAct, which the board profile already probes first.

---

## 2026-09-20 — Phase 4: CPU and SRAM measurements

**Done**
- `src/core/bench.[ch]`: the free heap region (nothing in this firmware allocates) is the working area for memory tests - RP2040 ~150 KB, RP2350 ~330 KB. Helpers for bank groups (RP2040: SRAM0-3; RP2350: SRAM0-3 and SRAM4-7), a deterministic xorshift PRNG, and the rule that measurement loops live in SRAM.
- `src/tests/cpu.c` (201-205): per-operation cost of add/and-or/shift/mul/udiv/sdiv against an empty loop of the same shape, reported in milli-cycles per op; predictable vs data-dependent branches; memcpy/memset/32-bit copy loop for 64 B to 16 KB with kB/s; software CRC-32; random and sequential lookups in 4/16/64 KB tables (the shape of the cartridge decode).
- `src/tests/sram_integrity.c` (300): constants 00/FF/55/AA, walking ones and zeros, address and inverted address, LFSR, byte and halfword passes, and March C- over every free byte of each bank group. Errors are recorded with address, expected and actual.
- `src/tests/sram_bench.c` (301-310): 8/16/32-bit sequential read/write per bank group (plus reverse and, on Cortex-M33 only, misaligned 32-bit); stride sweep 4..256 bytes to expose bank conflicts; dependent-load latency (pointer chase) for 4 KB to 256 KB; CPU loop with a DMA stream in the same and in the other bank group.
- Record arena enlarged (RP2040 48 KB, RP2350 96 KB): a full run with sweeps is now several hundred records.
- Registry: 26 tests, 5 of them swept over all five operating points -> 45 stages.

**Design notes**
- Unaligned 32-bit access is measured only on Cortex-M33; it faults on Cortex-M0+ and is not supported by Hazard3, so those builds record why instead.
- The pointer chase uses Sattolo's algorithm, which builds a single cycle over all entries directly in "next index" form. A Fisher-Yates permutation converted in place would have corrupted itself.
- Continuous DMA traffic uses one channel with an address ring (4 KB) and a fixed opposite address. A self-chaining pair of channels would restart with already-advanced addresses and walk out of the buffer.
- Scratch banks are not tested: they hold the stacks. Testing them needs the stack moved first (later phase).

**Hardware verification (WeAct RP2350B, RUN0011) - found three benchmark bugs**
- The run completed 45/45 stages with 498 records and **5 watchdog crashes, all in `sram.throughput`** (one per operating point). Crash recovery worked every time and the earlier results survived, which is a good end-to-end proof of the phase 1 design.
- **`sram.integrity`: 0 errors** over 70 KB in SRAM0-3 plus 256 KB in SRAM4-7 (const_00 pass: 1.4 ms and 5.2 ms).
- Valid numbers from that run (writes were not affected by the bug): write32 340 MB/s, write8 200 MB/s in group 0; group 1 write32 109 MB/s (that measurement was taken at 48 MHz, so it scales); memcpy 4 KB 168 MB/s, memset 16 KB 298 MB/s.

**Bugs found and fixed (verified in the disassembly)**
1. **Hang**: the misaligned test used a plain `uint32_t *` at an odd address. At -O3 the compiler emitted `LDRD`/`LDM`, which require alignment and fault on Cortex-M33 - the watchdog then reset the chip in every sweep. Now read through a `packed` type (single LDR, verified: no ldrd/ldm in the kernel), and only on M33.
2. **Reads reported 0 cycles**: read-only loops were inlined, proved loop-invariant and hoisted out of the timed region. Now called through a `volatile` function pointer with the arguments passed through a register barrier (verified: `blx` in `sample_access`, 8 `ldr` in `read32`). This also invalidated the DMA contention result (0 % slowdown, since the baseline was 0).
3. **Per-operation costs were nonsense** (udiv 0.125 cycles): the repeated expression was merged algebraically, and the constant divisor was strength-reduced to multiply+shift. Now a register fence sits between every operation and the divisor is opaque (verified: 8 adds / 8 muls / 8 udiv / 8 sdiv / 8 ror per iteration, and 8 calls to the SIO divider on RP2040).
- Also unrolled the stride and pointer-chase loops: with a tight loop the overhead hid the differences (stride 4 and stride 16 measured identically at 7.0 cycles).
- The lessons are written up in CLAUDE.md under "Measurement pitfalls".

**Hardware verification after the fixes (all three boards, 45/45 stages, 0 crashes, 0 errors)**
- WeAct RP2350B (RUN0013, 588 records), Pico 2 clone RP2350A (RUN0003, 588), purple RP2040 clone (RUN0001, 450).
- `sram.integrity`: zero errors on every board.

| measurement (default clock) | RP2350 @150 MHz | RP2040 @125 MHz |
|---|---|---|
| read32 sequential | 238.6 MB/s | 142.0 MB/s |
| write32 sequential | 366.6 MB/s | 188.6 MB/s |
| read16 / read8 | 119.6 / 53.3 MB/s | 71.2 / 34.4 MB/s |
| write16 / write8 | 265.2 / 159.4 MB/s | 153.0 / 110.7 MB/s |
| read32 misaligned | 164.7 MB/s (-31 %) | faults: not measured |
| dependent load (pointer chase) | 2.26 cycles | 3.52 cycles |
| add / and-or / shift | 1.00 cycle each | 1.00 cycle each |
| mul32 | 1.88 cycles | 1.00 cycle |
| udiv32 / sdiv32 | 4.00 cycles | **33.0 cycles** (SIO divider through a function call) |
| memcpy 4 KB | see CSV | see CSV |

- **The two RP2350 boards produced identical SRAM and CPU numbers to the last digit** (same core, same clock, no caches in the path) - the A2 clone is not slower than the WeAct anywhere except flash XIP.
- **Per-op costs are identical at every operating point**, i.e. they scale exactly with the clock, as they should.
- **RP2040 single-cycle multiply** (M0+ on RP2040 has the fast multiplier) but **division costs 33 cycles** including the call into the SIO divider helper - worth avoiding in the cartridge hot path.
- Writes are faster than reads on both chips (posted writes vs a stalling load).
- **Stride 4..256 bytes: no difference at all** (RP2350 2.66 cycles/access, RP2040 5.05). A single CPU issues one load at a time, so bank striping does not help or hurt it. The test now records `bank_conflict_effect_percent` and states this conclusion in the report itself.
- **DMA contention: 0 % on RP2350, 7 % on RP2040** - but the old test could not prove the DMA was running at all (one channel with an address ring, traffic on one side only). Reworked: an SRAM->SRAM copy is restarted for every sample outside the timed region, and the words actually transferred during the samples are counted and reported (`dma_words_*`), with an error recorded if it is zero. **Needs a re-test to confirm the 0 %.**

**Next**
- Re-test the contention numbers, then phase 5 (flash/XIP/cache), which should also carry the XIP mode + clock sweep discussed for the dual-mode clone.

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
