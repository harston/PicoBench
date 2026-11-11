# ARM workload (local, not in the repository)

Two tests replay real ARM code of Atari 2600 Harmony cartridge games (DPC+, CDF, CDFJ,
CDFJ+) through a Thumb interpreter:

- `cpu.thumb_interp`: how fast each chip interprets that code, and whether it is fast
  enough for each game,
- `hotpath.arm_neighbour`: what the interpreter running on one core does to the latency
  of a cartridge hot path on the other core.

That code belongs to the games' authors, so it is **not distributed** with PicoBench.
You generate it from ROM files you own. It goes into this directory, which git ignores
except for this README. Without it the firmware builds normally: `cpu.thumb_interp`
reports `N/A`, and `hotpath.arm_neighbour` still runs with a synthetic ARM load written
for PicoBench (`src/tests/arm/synth_load.h`) - the game code only adds a comparison
variant, `neighbour_game_same`.

## Generating it

Needs Python 3, a C compiler, git and Go.

```sh
# 1. Build the recorder: the Gopher2600 emulator with a patch that records ARM calls
#    (clones Gopher2600 at the pinned commit; creates workload/.tools/armstat)
worklog/tools/arm_workload/recorder/build_recorder.sh

# 2. See which catalogue titles your ROMs are (identified by CRC32, not by name)
worklog/tools/arm_workload/awl_tool.py scan ~/roms

# 3. Record them -> workload/arm_workload.c
worklog/tools/arm_workload/awl_tool.py build ~/roms                 # reference set
worklog/tools/arm_workload/awl_tool.py build ~/roms --titles all     # every catalogue title found
worklog/tools/arm_workload/awl_tool.py build ~/roms --titles galagon,mappy
worklog/tools/arm_workload/awl_tool.py build ~/roms --crc 4f8184d7,935617de
worklog/tools/arm_workload/awl_tool.py build --add mygame.bin:mygame --sweep   # any ARM cartridge

# 4. Build: plain UF2s stay free of game data, *_local.uf2 get the workload
./build.sh
```

`awl_tool.py list` shows the catalogue (`worklog/tools/arm_workload/titles.csv`): 27
titles with the ROM CRC32, the ARM format and the MIPS each game needs for a stable
frame. The **reference set** (7 titles, `set=ref`) is the one PicoBench's own results
were measured with.

## How a title is recorded

1. The recorder runs the ROM without a window, with scripted input (RESET, FIRE, then
   random joystick moves from a fixed seed). It records ten consecutive ARM calls from
   frame 2400 on: registers, the cartridge RAM before and after each call, and every
   driver call and peripheral read.
2. `replay.c` runs every call through the firmware's own interpreter
   (`src/tests/arm/thumb_core.c`) and checks the result against the emulator's bit for
   bit.
3. It then zeroes the ROM one 256-byte page at a time and keeps only the pages the
   calls need (4-37 KB per game instead of the whole ROM).
4. For a catalogue title the result must match the checksum in `titles.csv`. The
   recording is deterministic, so the same ROM always gives a byte-identical workload.

The firmware reports `workload_crc32` in `cpu.thumb_interp`. Two runs measured the
same code only if that value matches. The reference set is `5354c6e4`.

For a game outside the catalogue, `--sweep` measures the MIPS it needs by lowering the
ARM clock in the emulator's Harmony timing model until frames break. That is up to ten
3600-frame runs, a few minutes per game. Without it the need is "unknown" and the test
reports the speed only. The tool prints a `catalogue line:` for every new title; that
line holds no game data, so it can be added to `titles.csv` and shared.

## Keep it local

`workload/arm_workload.c` and the `dist/*_local.uf2` images contain excerpts of the
games. Do not publish them. git ignores both, and `build.sh` never puts the workload
into the plain `dist/picobench_<chip>.uf2` images.
