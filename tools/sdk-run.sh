#!/usr/bin/env bash
#
# Build a real MCUXpresso SDK example from source and run it on the emulator.
#
# This is the "prove customer code runs" harness: it compiles an unmodified NXP
# SDK example with the actual fsl_* drivers, then boots it under our QEMU model
# and captures the console + any unimplemented-register accesses.
#
# SETUP (once):
#   1. Extract the full SDK zip:   unzip SDK_..._MIMXRT1180-EVK.zip -d $SDK_ROOT
#   2. Build deps in a venv:       python3 -m venv $VENV
#                                  $VENV/bin/pip install west kconfiglib \
#                                      ruamel.yaml pyyaml jinja2 pyelftools \
#                                      pykwalify canopen packaging progress psutil
#   3. Have arm-none-eabi-gcc on PATH (ARMGCC_DIR points at its prefix, e.g. /usr).
#
# USAGE:
#   SDK_ROOT=/path/to/sdk/mcuxsdk VENV=/path/to/venv \
#     tools/sdk-run.sh driver_examples/lpit/single_channel [cm33|cm7]
#
# NOTE: builds with `--config debug` (links to TCM/RAM), NOT the default
# flexspi_nor XIP config -- the XIP images assume the boot ROM set up FlexSPI
# execute-in-place, which the direct -kernel loader does not replicate.  The
# driver code exercised is identical; only the link/boot differs.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
SDK_ROOT="${SDK_ROOT:?set SDK_ROOT to the extracted .../sdk/mcuxsdk}"
VENV="${VENV:?set VENV to the python venv with west+kconfiglib}"
export ARMGCC_DIR="${ARMGCC_DIR:-/usr}"
export PATH="$VENV/bin:$PATH"

EXAMPLE="${1:?usage: sdk-run.sh <examples-relative-path> [core]}"
CORE="${2:-cm33}"
NAME="$(echo "$EXAMPLE" | tr '/' '_')_$CORE"
BUILD="/tmp/sdkbuild_$NAME"

echo ">> building examples/$EXAMPLE ($CORE, --config debug/RAM)"
# west's mcux 'build' extension is only available inside the SDK workspace.
if ! ( cd "$SDK_ROOT" && "$VENV/bin/west" build -b evkmimxrt1180 --toolchain armgcc \
        "examples/$EXAMPLE" -Dcore_id="$CORE" --config debug \
        -d "$BUILD" ) >/tmp/sdk_$NAME.log 2>&1; then
    echo "   BUILD FAILED (see /tmp/sdk_$NAME.log)"; tail -3 /tmp/sdk_$NAME.log; exit 1
fi
ELF="$(ls "$BUILD"/*.elf 2>/dev/null | head -1)"
[ -n "$ELF" ] || { echo "   no ELF produced"; exit 1; }

echo ">> running on $QEMU"
timeout 12 "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
    -kernel "$ELF" -serial "file:/tmp/sdk_$NAME.con" \
    -semihosting-config enable=on,target=native -d unimp 2>/tmp/sdk_$NAME.unimp
echo "---- console ----"
cat "/tmp/sdk_$NAME.con"
echo "---- unimplemented blocks touched (family bases) ----"
grep -oiE 'offset 0x[0-9a-f]+' "/tmp/sdk_$NAME.unimp" 2>/dev/null \
    | sed -E 's/(0x....).*/\10000 (abs 0x4\1000)/' | sort | uniq -c | sort -rn | head
