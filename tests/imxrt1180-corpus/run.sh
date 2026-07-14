#!/bin/bash
# Saturation harness: boot every prebuilt MCUXpresso SDK cm33 demo on the
# mimxrt1180-evk machine and report pass / hang / fault.  A quick way to
# regression-check the model against a corpus of real NXP firmware.
#
# The prebuilt .bin demos ship inside the EVK SDK zip (kept out of git under
# reference/).  Point SDK_ZIP at it, or pre-extract into reference/sdk-fw/.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
FWDIR="${FWDIR:-$ROOT/reference/sdk-fw}"
SDK_ZIP="${SDK_ZIP:-$ROOT/reference/SDK_26_06_00_MIMXRT1180-EVK.zip}"
TIMEOUT="${TIMEOUT:-10}"

# Extract the prebuilt cm33 demo bins from the SDK zip if not already present.
if [ ! -d "$FWDIR" ] || [ -z "$(find "$FWDIR" -ipath '*evkmimxrt1180*cm33*.bin' 2>/dev/null)" ]; then
    echo "Extracting prebuilt cm33 demos from $SDK_ZIP ..."
    unzip -o -q "$SDK_ZIP" "*evkmimxrt1180*cm33*.bin" -d "$FWDIR" || {
        echo "ERROR: could not extract demos; set SDK_ZIP to the EVK SDK zip." >&2
        exit 1
    }
fi

printf "%-34s %-8s %s\n" "DEMO" "RESULT" "NOTE"
printf "%-34s %-8s %s\n" "----" "------" "----"
find "$FWDIR" -ipath '*evkmimxrt1180*cm33*.bin' | sort -u | while read -r bin; do
    name=$(echo "$bin" | sed -E 's|.*evkmimxrt1180/||; s|/cm33.*||')
    out=$(mktemp); err=$(mktemp)
    timeout -k 5 "$TIMEOUT" "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
        -kernel "$bin" -serial stdio -semihosting-config enable=on,target=native \
        >"$out" 2>"$err" </dev/null
    rc=$?
    console=$(tr -d '\0' <"$out" | tr '\n' ' ' | sed -E 's/  +/ /g' | cut -c1-44)
    if grep -qiE "Lockup|fatal|Aborted" "$err"; then
        res="FAULT"    # guest lockup / qemu abort (missing peripheral)
    elif grep -qiE "assert" "$out"; then
        res="ASSERT"   # firmware assertion (missing/zero register)
    elif [ "$rc" -eq 124 ]; then
        res="RUN"      # still running at timeout (loop/blink/wait) or hang
    else
        res="EXIT"     # semihosting exit
    fi
    printf "%-34s %-8s [%s]\n" "$name" "$res" "$console"
    rm -f "$out" "$err"
done
