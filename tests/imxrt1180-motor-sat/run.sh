#!/usr/bin/env bash
#
# RT1180 virtual-motor plant — MAGNETIC SATURATION value-test.
#
# Saturation has no static signature, so this rides the current TRANSIENT: the
# d-axis rise-time constant tau=Ld/Rs is current-independent when linear but
# shrinks with current when the iron saturates. The firmware times the rise to
# 50% of steady for a SMALL and a LARGE d-axis step (rotor stationary at theta=0);
# the RATIO t_large/t_small is 1.0 linear, <1 saturating, and cancels the SysTick
# rate. We SWEEP i_sat and assert the ratio matches the closed form
#   t(i) = Ld0 * [i_sat/(vd+Rs*i_sat)] * ln[(1+i/i_sat)/(1-Rs*i/vd)],  i=0.5*vd/Rs
# (Ld0 cancels in the ratio). Rs/Vbus/CUR_FS are the SDK M1 + board constants,
# hardcoded independently of the plant. Needs -icount + a high plant rate.
# Mutation-proven.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
case "$QEMU" in /*) ;; *) QEMU="$PWD/$QEMU" ;; esac    # make -C runs in the test dir

for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
make -C "$HERE" sat.elf QEMU="$QEMU" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
ELF="$HERE/sat.elf"

# Sweep i_sat in mA (0 = saturation OFF -> linear -> ratio 1.0).
ISATS=(0 10000 5000 3000)
results=""
for isat in "${ISATS[@]}"; do
    for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
    out="$(timeout -k 3 40 "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
        -kernel "$ELF" -serial null -semihosting-config enable=on,target=native -icount shift=3 \
        -global imxrt1180-motor.rate-hz=500000 -global imxrt1180-motor.sat-isat-ma=$isat </dev/null 2>&1)"
    ts="$(echo "$out" | grep -oE 'ts=[0-9]+' | head -1 | cut -d= -f2)"
    tl="$(echo "$out" | grep -oE 'tl=[0-9]+' | head -1 | cut -d= -f2)"
    results+="$isat ${ts:-NA} ${tl:-NA};"
done

echo "$results" | python3 -c '
import sys, math
Rs, Vbus, Xs, Xl = 0.54, 24.0, 40, 320       # SDK M1 + board constants
vds, vdl = Xs/1000*Vbus, Xl/1000*Vbus
TOL = 0.05
def t_over_Ld(vd, isat):
    i = 0.5*vd/Rs                            # 50% of steady
    if isat <= 0: return math.log(2)/Rs      # linear
    C = isat/(vd + Rs*isat)
    return C*math.log((1+i/isat)/(1-Rs*i/vd))
ok = True
for r in [x for x in sys.stdin.read().strip().split(";") if x]:
    isat_ma, ts, tl = r.split()
    isat = int(isat_ma)/1000.0
    golden = t_over_Ld(vdl, isat) / t_over_Ld(vds, isat)
    if ts == "NA" or tl == "NA":
        print(f"  i_sat={isat_ma}mA: golden {golden:.3f} -> NO OUTPUT  FAIL"); ok=False; continue
    meas = int(tl)/int(ts)
    err = abs(meas-golden)/golden
    v = "ok" if err <= TOL else "FAIL"
    if err > TOL: ok = False
    print(f"  i_sat={isat_ma:>5}mA: golden ratio {golden:.3f}  measured {meas:.3f}  ({err*100:.1f}%)  {v}")
sys.exit(0 if ok else 1)
'
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "MOTOR-SAT: PASS (d-axis rise-time ratio matches the saturating-inductance closed form across the i_sat sweep)"
else
    echo "MOTOR-SAT: FAIL"
fi
exit "$rc"
