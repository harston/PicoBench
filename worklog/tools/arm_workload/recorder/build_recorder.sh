#!/usr/bin/env bash
# Build armstat: the Gopher2600 emulator with the instrumentation patch that records
# ARM calls (ARMREC01 files). awl_tool.py uses it to record the ARM workload.
#
# usage: build_recorder.sh [GOPHER2600_CHECKOUT]
#   GOPHER2600_CHECKOUT  an existing clone to copy from (it is not modified);
#                        default: clone https://github.com/JetSetIlly/Gopher2600
#
# Needs git and Go (>= the version in Gopher2600's go.mod). The first build downloads
# Gopher2600's Go modules. Output: workload/.tools/armstat
#
# The patch is made against Gopher2600 commit PINNED below. The catalogue checksums in
# titles.csv were produced with exactly that commit; another one may record different
# calls, which awl_tool.py reports as a checksum mismatch.
#
# Source of truth for main.go and the patch: PicoA10400 tools/arm_carts/armstat/.
# Keep the copies identical.
set -euo pipefail

PINNED=d97a9391bc2466c7eed8b9ab180c9f6c9308b1d3
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
TOOLS="$ROOT/workload/.tools"
SRC="$TOOLS/gopher2600"

mkdir -p "$TOOLS"
rm -rf "$SRC"
if [ $# -ge 1 ]; then
    git clone -q "$1" "$SRC"
else
    git clone -q https://github.com/JetSetIlly/Gopher2600 "$SRC"
fi
git -C "$SRC" -c advice.detachedHead=false checkout -q "$PINNED"
(cd "$SRC" && patch -p1 -s < "$HERE/gopher2600_instrumentation.patch")
mkdir -p "$SRC/cmd/armstat"
cp "$HERE/main.go" "$SRC/cmd/armstat/"
(cd "$SRC" && go build -o "$TOOLS/armstat" ./cmd/armstat)
echo "built $TOOLS/armstat (Gopher2600 ${PINNED:0:8} + instrumentation patch)"
