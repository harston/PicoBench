#!/usr/bin/env bash
# Build PicoBench for every platform and collect the UF2s in dist/.
#
# Usage: ./build.sh [platform...]      (default: rp2040 rp2350-arm-s rp2350-riscv)
#
# Variants (PICOBENCH_VARIANT):
#   (unset)     normal firmware              -> dist/picobench_<chip>.uf2
#   crashtest   adds the debug.crash test    -> dist/picobench_<chip>_crashtest.uf2
#               (hangs on purpose to exercise watchdog recovery)
#   overclock   short run with the overclocking explorer, OUTSIDE the chips'
#               specification                -> dist/picobench_<chip>_overclock.uf2
#               (published, at the user's own risk; see README "Overclocking variant")
#
# ARM workload (cpu.thumb_interp, hotpath.arm_neighbour): excerpts of commercial games,
# generated locally into workload/arm_workload.c (workload/README.md) and never
# committed. dist/picobench_<chip>.uf2 is ALWAYS built without it, so dist/ can be
# published. When the file exists, a second image with it is built:
#   dist/picobench_<chip>_local.uf2   (git-ignored - do not publish)
# The crashtest variant includes the workload when present (its UF2s are git-ignored).
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

VARIANT="${PICOBENCH_VARIANT:-}"
WORKLOAD="$ROOT/workload/arm_workload.c"
EXTRA_ARGS=()
SUFFIX=""
case "$VARIANT" in
    "") EXTRA_ARGS+=(-DPICOBENCH_ARM_WORKLOAD=none) ;;
    crashtest) EXTRA_ARGS+=(-DPICOBENCH_TEST_CRASH=ON -DPICOBENCH_ARM_WORKLOAD=auto); SUFFIX="_crashtest" ;;
    overclock) EXTRA_ARGS+=(-DPICOBENCH_OVERCLOCK=ON -DPICOBENCH_ARM_WORKLOAD=none); SUFFIX="_overclock" ;;
    *) echo "unknown PICOBENCH_VARIANT: $VARIANT" >&2; exit 1 ;;
esac

PLATFORMS=("$@")
[ ${#PLATFORMS[@]} -eq 0 ] && PLATFORMS=(rp2040 rp2350-arm-s rp2350-riscv)

uf2_name() {   # platform suffix
    case "$1" in
        rp2040)       echo "picobench_rp2040$2.uf2" ;;
        rp2350-arm-s) echo "picobench_rp2350_arm$2.uf2" ;;
        rp2350-riscv) echo "picobench_rp2350_riscv$2.uf2" ;;
        *) echo "unknown platform: $1" >&2; exit 1 ;;
    esac
}

mkdir -p "$ROOT/dist"
STAMP="$(date +%Y%m%d-%H%M%S)"

build_one() {   # platform build-dir uf2-name cmake-args...
    local plat="$1" bdir="$2" name="$3" tc
    shift 3
    case "$plat" in
        rp2350-riscv) tc="$RISCV_TC" ;;
        *)            tc="$ARM_TC" ;;
    esac
    echo "=== $plat -> dist/$name"
    local log
    log="$(cmake -S "$ROOT" -B "$bdir" -G Ninja \
        -DPICO_PLATFORM="$plat" \
        -DPICO_TOOLCHAIN_PATH="$tc" \
        -DCMAKE_BUILD_TYPE=Release "$@" 2>&1)" || { echo "$log" >&2; exit 1; }
    grep "ARM workload\|variant" <<<"$log" || true
    cmake --build "$bdir"

    if [ -f "$ROOT/dist/$name" ]; then
        mkdir -p "$ROOT/dist/backup/$STAMP"
        cp -p "$ROOT/dist/$name" "$ROOT/dist/backup/$STAMP/$name"
    fi
    cp "$bdir/picobench.uf2" "$ROOT/dist/$name"
}

for plat in "${PLATFORMS[@]}"; do
    build_one "$plat" "$ROOT/build/$plat${VARIANT:+-$VARIANT}" "$(uf2_name "$plat" "$SUFFIX")" \
        ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
    if [ -z "$VARIANT" ] && [ -f "$WORKLOAD" ]; then
        build_one "$plat" "$ROOT/build/$plat-local" "$(uf2_name "$plat" _local)" \
            -DPICOBENCH_ARM_WORKLOAD="$WORKLOAD"
    fi
done

echo
ls -l "$ROOT/dist"/*.uf2
