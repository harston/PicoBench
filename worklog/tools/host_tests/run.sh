#!/usr/bin/env bash
# Build and run host-side tests of platform-independent firmware code:
#   test_core   stats + CRC32
#   test_awl    synthetic ARM load of hotpath.arm_neighbour (every build), and the
#               ARM workload (workload/arm_workload.c, if present) replayed through
#               the Thumb interpreter; the empty stub must read as "no workload"
#   test_store  results store (incl. power loss at every flash op) + virtual FAT disk
# Then checks the generated 16 MB FAT16 image with fsck_msdos and, on macOS, mounts
# it read-only and compares the latest run's files byte for byte.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
HT="$ROOT/worklog/tools/host_tests"
OUT="${TMPDIR:-/tmp}/picobench_host_tests"
mkdir -p "$OUT"
CFLAGS=(-std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined)

echo "== test_core"
cc "${CFLAGS[@]}" -I"$ROOT/src" "$HT/test_core.c" \
    "$ROOT/src/core/stats.c" "$ROOT/src/results/crc32.c" -o "$OUT/test_core"
"$OUT/test_core"

echo "== test_awl (ARM workload through the Thumb interpreter)"
AWL_SRC=("$HT/test_awl.c" "$ROOT/src/tests/arm/awl.c" "$ROOT/src/tests/arm/thumb_core.c"
         "$ROOT/src/tests/arm/synth_load.c" "$ROOT/src/results/crc32.c")
cc "${CFLAGS[@]}" -Wno-unused-label -I"$ROOT/src" "${AWL_SRC[@]}" \
    "$ROOT/src/tests/arm/arm_workload_none.c" -o "$OUT/test_awl_none"
"$OUT/test_awl_none" --none
# The workload itself is local only (game data, workload/README.md).
if [ -f "$ROOT/workload/arm_workload.c" ]; then
    cc "${CFLAGS[@]}" -Wno-unused-label -I"$ROOT/src" "${AWL_SRC[@]}" \
        "$ROOT/workload/arm_workload.c" -o "$OUT/test_awl"
    "$OUT/test_awl"
else
    echo "no workload/arm_workload.c - replay of the workload skipped"
fi

echo "== test_store"
cc "${CFLAGS[@]}" -DPICOBENCH_HOST_TEST -I"$HT/stubs" -I"$ROOT/src" -I"$ROOT" "$HT/test_store.c" \
    "$ROOT/src/storage/results_store.c" "$ROOT/src/usb/msc_disk.c" \
    "$ROOT/src/results/crc32.c" "$ROOT/src/results/sink.c" -o "$OUT/test_store"
OUT_IMG="$OUT/disk.img" "$OUT/test_store"

echo "== fsck_msdos"
if command -v fsck_msdos >/dev/null; then
    fsck_msdos -n "$OUT/disk.img"
else
    echo "fsck_msdos not available, skipped"
fi

if [ "$(uname)" = Darwin ]; then
    echo "== mount (read-only) and compare"
    MNT="$OUT/mnt"
    mkdir -p "$MNT"
    DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -readonly -nobrowse \
          -mountpoint "$MNT" "$OUT/disk.img" | awk 'NR==1{print $1}')
    trap 'hdiutil detach "$DEV" >/dev/null 2>&1 || true' EXIT
    ls -la "$MNT" "$MNT/RUNS" "$MNT/.fseventsd" | head -40
    for f in "$OUT"/disk.img.expect.*; do
        name="${f##*.expect.}"
        cmp "$f" "$MNT/$name" && echo "  $name identical"
    done
    n=$(ls "$MNT/RUNS" | wc -l | tr -d ' ')
    echo "  RUNS/ holds $n run directories"
fi
echo "ALL HOST TESTS PASSED"
