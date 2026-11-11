# Third-party notices

PicoBench is licensed under GPL-3.0-or-later (see [LICENSE](LICENSE)). This file lists
code by others that is either kept in this repository or linked into the UF2 firmware
images in `dist/`, together with the notices their licenses require. All of these
licenses are compatible with GPL-3.0.

## Code in this repository

| Path | Origin | License |
|---|---|---|
| `cmake/pico_sdk_import.cmake` | Raspberry Pi Pico SDK, copied unchanged | BSD-3-Clause (notice below, also kept in the file) |
| `src/tests/arm/thumb_core.c`, `src/tests/arm/thumb_core.h` | Verbatim copy of the Thumb interpreter from the PicoA10400 project (`tools/arm_carts/thumb/`). Its instruction semantics follow the ARM7TDMI model of [Gopher2600](https://github.com/JetSetIlly/Gopher2600) by JetSetIlly (`hardware/memory/cartridge/arm/thumb.go`, commit `d97a9391`), which is licensed GPL-3.0-or-later | GPL-3.0-or-later |
| `worklog/tools/arm_workload/replay.c` | Verbatim copy from the PicoA10400 project (`tools/arm_carts/thumb/`) | GPL-3.0-or-later |
| `worklog/tools/arm_workload/recorder/main.go`, `gopher2600_instrumentation.patch` | Copies from the PicoA10400 project (`tools/arm_carts/armstat/`): a program built on Gopher2600 and a patch to Gopher2600 itself. `build_recorder.sh` downloads Gopher2600 from GitHub at build time; it is not part of this repository | GPL-3.0-or-later, like Gopher2600 |

## Game data - not included

The ARM workload tests use code and data of Atari 2600 Harmony cartridge games. Those
belong to their authors and are **not** part of this repository or of the UF2 images in
`dist/`. Users generate them locally from their own ROM files
(`workload/README.md`). The catalogue `worklog/tools/arm_workload/titles.csv` holds only
titles, CRC32 checksums and measured figures, no game content.

## Code linked into the firmware images

The UF2 images are built with the Raspberry Pi Pico SDK 2.3.0 and include:

| Component | License |
|---|---|
| Raspberry Pi Pico SDK (hardware drivers, runtime, boot stage 2, flash and clock support) | BSD-3-Clause |
| TinyUSB (USB device stack: MSC disk and CDC console), from `pico-sdk/lib/tinyusb` | MIT |
| pico_printf (`snprintf`/`vsnprintf`), based on the printf of Marco Paland | MIT |
| newlib C library (`memcpy`, `memset`, string functions) | BSD-style licenses, see `COPYING.NEWLIB` in the toolchain |
| libgcc (compiler support routines) | GPL-3.0 with the GCC Runtime Library Exception |

---

### Raspberry Pi Pico SDK - BSD-3-Clause

```
Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### TinyUSB - MIT

```
The MIT License (MIT)

Copyright (c) 2018, hathach (tinyusb.org)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### printf (used by pico_printf) - MIT

```
The MIT License (MIT)

Copyright (c) 2014 Marco Paland

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
