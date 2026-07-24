#!/usr/bin/env bash
#
# RT1180 virtual-motor plant — speed-squared (fan) LOAD value-test.
#
# Seeds the rotor with a speed and lets it coast to rest (no drive) under the fan
# load + viscous damping. Asserts the TOTAL coast-down angle (final encoder count)
# matches the closed form  theta = (J/k) ln(1 + k*w0/B)  across a SWEEP of both the
# seed speed w0 AND the load coefficient k -- one wrong shape cannot pass. The
# golden's J/B/CPR are the SDK M1 motor params, hardcoded here independently of the
# plant's #defines, so a mutation of either the load law or J is caught.
# Needs -icount (deterministic physics vs CPU rate). Mutation-proven.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
case "$QEMU" in /*) ;; *) QEMU="$PWD/$QEMU" ;; esac    # make -C runs in the test dir

for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
make -C "$HERE" load.elf QEMU="$QEMU" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
ELF="$HERE/load.elf"

# Sweep: "init_mrads load_fan_unms" (w0 in milli-rad/s, k in micro-N*m/(rad/s)^2).
CONFIGS=("150000 10" "100000 10" "200000 20" "150000 50" "250000 30")
results=""
for cfg in "${CONFIGS[@]}"; do
    set -- $cfg
    for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
    out="$(timeout -k 3 30 "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
        -kernel "$ELF" -serial null -semihosting-config enable=on,target=native -icount shift=3 \
        -global imxrt1180-motor.init-mrads=$1 -global imxrt1180-motor.load-fan-unms=$2 </dev/null 2>&1)"
    tc="$(echo "$out" | grep -oE 'total_counts=-?[0-9]+' | head -1 | cut -d= -f2)"
    results+="$1 $2 ${tc:-NA};"
done

echo "$results" | python3 -c '
import sys, math
# SDK M1 motor params (m1_pmsm_appconfig.h / MCAT), independent of the plant code.
J, B, CPR = 1e-5, 1e-4, 8000
TOL = 0.04                      # 4% (dormant-threshold cutoff + Euler discretisation)
rows = [r for r in sys.stdin.read().strip().split(";") if r]
ok = True
for r in rows:
    w0_m, unms, tc = r.split()
    w0 = int(w0_m) / 1000.0
    k  = int(unms) / 1e6
    golden = (J / k) * math.log(1.0 + k * w0 / B) * CPR / (2 * math.pi)
    if tc == "NA":
        print(f"  w0={w0:.0f} k={k:.0e}: golden {golden:.0f} -> NO OUTPUT  FAIL"); ok = False; continue
    meas = int(tc)
    err = abs(meas - golden) / golden
    verdict = "ok" if err <= TOL else "FAIL"
    if err > TOL: ok = False
    print(f"  w0={w0:.0f} rad/s  k={k:.0e}: golden {golden:.0f}  measured {meas}  ({err*100:.1f}%)  {verdict}")
sys.exit(0 if ok else 1)
'
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "MOTOR-LOAD: PASS (coast-down angle matches (J/k)ln(1+k*w0/B) across the w0/k sweep)"
else
    echo "MOTOR-LOAD: FAIL"
fi
exit "$rc"
