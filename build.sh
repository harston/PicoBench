#!/usr/bin/env bash
# Build PicoBench for every platform and collect the UF2s in dist/.
#
# Usage: ./build.sh [platform...]      (default: rp2040 rp2350-arm-s rp2350-riscv)
#
# Environment:
#   PICO_SDK_PATH      Pico SDK 2.3.0 checkout (default: ~/pico/pico-sdk)
#   PICOBENCH_ARM_TC   ARM gcc root (default: Arduino core pqt-gcc)
#   PICOBENCH_RISCV_TC RISC-V gcc root (default: Arduino core pqt-gcc-riscv)
#
# Existing UF2s in dist/ are copied to dist/backup/<timestamp>/ before being replaced:
# they are what gets flashed to hardware.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
export PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/pico/pico-sdk}"
PQT="$HOME/Library/Arduino15/packages/rp2040/tools"
ARM_TC="${PICOBENCH_ARM_TC:-$PQT/pqt-gcc/4.1.0-1aec55e}"
RISCV_TC="${PICOBENCH_RISCV_TC:-$PQT/pqt-gcc-riscv/4.1.0-1aec55e}"

PLATFORMS=("$@")
[ ${#PLATFORMS[@]} -eq 0 ] && PLATFORMS=(rp2040 rp2350-arm-s rp2350-riscv)

uf2_name() {
    case "$1" in
        rp2040)       echo picobench_rp2040.uf2 ;;
        rp2350-arm-s) echo picobench_rp2350_arm.uf2 ;;
        rp2350-riscv) echo picobench_rp2350_riscv.uf2 ;;
        *) echo "unknown platform: $1" >&2; exit 1 ;;
    esac
}

mkdir -p "$ROOT/dist"
STAMP="$(date +%Y%m%d-%H%M%S)"

for plat in "${PLATFORMS[@]}"; do
    name="$(uf2_name "$plat")"
    case "$plat" in
        rp2350-riscv) tc="$RISCV_TC" ;;
        *)            tc="$ARM_TC" ;;
    esac
    bdir="$ROOT/build/$plat"
    echo "=== $plat -> dist/$name"
    cmake -S "$ROOT" -B "$bdir" -G Ninja \
        -DPICO_PLATFORM="$plat" \
        -DPICO_TOOLCHAIN_PATH="$tc" \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$bdir"

    if [ -f "$ROOT/dist/$name" ]; then
        mkdir -p "$ROOT/dist/backup/$STAMP"
        cp -p "$ROOT/dist/$name" "$ROOT/dist/backup/$STAMP/$name"
    fi
    cp "$bdir/picobench.uf2" "$ROOT/dist/$name"
done

echo
ls -l "$ROOT/dist"/*.uf2
