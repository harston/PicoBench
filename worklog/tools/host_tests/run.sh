#!/usr/bin/env bash
# Build and run host-side unit tests of platform-independent firmware code.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/picobench_host_tests"
cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
    -I"$ROOT/src" \
    "$ROOT/worklog/tools/host_tests/test_core.c" \
    "$ROOT/src/core/stats.c" \
    "$ROOT/src/results/crc32.c" \
    -o "$OUT"
"$OUT"
