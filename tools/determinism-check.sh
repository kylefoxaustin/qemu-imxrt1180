#!/usr/bin/env bash
#
# Determinism check — does the same input give the same output twice?
#
# "RUNG 3 REQUIRES A DETERMINISTIC INSTRUMENT. A correct golden compared against a
#  NOISY measurement produces a confident, reproducible-looking, WRONG answer --
#  and it will pass or fail depending on what else the machine is doing."
#                                                     -- mcxn947qemu, 2026-07-12
#
# ollama_95_neutron then ran every correctness check in their project exactly ONCE
# and discovered, when they finally repeated one, that the engine under test was
# NON-DETERMINISTIC: same graph, same input, a different answer every run. Their
# verdicts had survived only because the scatter was smaller than their tolerance
# -- luck, not design.
#
# "'I assumed it was deterministic' and 'I verified it is deterministic' are
#  precisely the two things this thread has spent 27 findings proving are
#  different."                                            -- orb_slam, 2026-07-12
#
# So: run each test N times and require the VERDICT to be identical every time. A
# flaky test is not a nuisance -- it is an instrument that reports a different
# answer for the same question, and every golden measured with it is a coin toss.
#
# NOTE this checks the VERDICT. A verdict can be stable while the underlying
# MEASUREMENT scatters, if the tolerance is wide enough to hide it -- which is
# exactly how ollama's survived. Where a test prints its measured value, check the
# VALUE too:
#   - tests/imxrt1180-pwm prints measured cycles, and runs under -icount, so it is
#     bit-exact (96000/96000/96000). WITHOUT -icount it jitters (96711/96207/95848)
#     and a tolerance wide enough to absorb that is too wide to catch real drift.
#   - tests/imxrt1180-motor prints di=8340 pos=256 identically every run, because
#     it measures a STEADY STATE -- an equilibrium is timing-independent by
#     construction. (The old test sampled a TRANSIENT, which would have scattered.
#     Measuring a converged value fixes correctness and determinism together.)
#
# Usage: tools/determinism-check.sh [runs]        (default 5)
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-5}"
flaky=0

echo "Running each test $N times; any variance in verdict is a FLAKY INSTRUMENT."
echo
for d in "$ROOT"/tests/imxrt1180-*/; do
    [ -f "$d/Makefile" ] || continue
    grep -q "^run:" "$d/Makefile" || continue
    t=$(basename "$d")
    pass=0; fail=0
    for _ in $(seq 1 "$N"); do
        o=$( (cd "$d" && timeout 180 make run 2>&1) )
        if echo "$o" | grep -q ": FAIL"; then fail=$((fail+1)); else pass=$((pass+1)); fi
    done
    if [ "$fail" -gt 0 ] && [ "$pass" -gt 0 ]; then
        printf "  %-24s PASS=%d FAIL=%d   *** NON-DETERMINISTIC ***\n" "$t" "$pass" "$fail"
        flaky=$((flaky+1))
    elif [ "$fail" -gt 0 ]; then
        printf "  %-24s FAIL=%d (consistently failing -- a real bug, not a flake)\n" "$t" "$fail"
        flaky=$((flaky+1))
    else
        printf "  %-24s PASS=%d\n" "$t" "$pass"
    fi
done

echo
if [ "$flaky" -gt 0 ]; then
    echo "$flaky test(s) are not deterministic. Every golden they measure is a coin toss."
    exit 1
fi
echo "All tests deterministic over $N runs."
