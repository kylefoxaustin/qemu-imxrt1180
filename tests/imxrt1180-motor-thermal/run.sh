#!/usr/bin/env bash
#
# RT1180 virtual-motor plant — winding-thermal value-test.
#
# Drives the plant open-loop (rotor aligns, resistive phase current) with the
# thermal model ENABLED, and asserts the phase current droops from the cold
# Ohm's-law value (4369) to the closed-form hot-Rs golden (3214, R_th=20 degC/W).
# The golden is derived from datasheet + test constants (Rs0, alpha, |v|, R_th),
# independent of the model code it checks. Mutation-proven (tools/mutation-audit
# style: inert thermal -> no droop; wrong tempco -> wrong droop; both FAIL).
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
# Absolutise QEMU: `make -C` runs in the test dir, so a relative path would break.
case "$QEMU" in /*) ;; *) QEMU="$PWD/$QEMU" ;; esac

# Kill any stale QEMU (comm-match via pidof, never `pkill -f` which self-matches
# this script's own command line) so a not-yet-dead instance can't block ours.
for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done

make -C "$HERE" QEMU="$QEMU" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

out="$(make -C "$HERE" run QEMU="$QEMU" 2>&1)"
echo "$out" | grep -E 'THERMAL:' || true

if echo "$out" | grep -q 'THERMAL: PASS'; then
    echo "MOTOR-THERMAL: PASS (phase current droops to the closed-form hot-Rs golden)"
    exit 0
fi
echo "MOTOR-THERMAL: FAIL"
exit 1
