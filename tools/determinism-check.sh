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
# ⚠️ AND RUN IT UNDER LOAD. A green suite on an IDLE box is a measurement of the
# box. mcxn947qemu's ztest passed 4/4 idle and FAILED REPRODUCIBLY under host load,
# on the same tree -- the failure was the HARNESS, not the model. I made exactly
# that mistake: I declared this suite "deterministic" from an idle run, and under
# saturation TWO tests failed:
#   - imxrt1180-timers2 compared two point-samples of PERIODIC counters (they
#     coincide by phase). The IDENTICAL defect I had already fixed in
#     imxrt1180-tmr -- and never grepped for elsewhere. Fixing the instance you
#     are looking at is not fixing the bug.
#   - imxrt1180-uartlink polled for a result for a fixed 10 s of WALL CLOCK and
#     called a slow box a failing model. A test whose verdict depends on how busy
#     the machine is has no verdict.
#
# Usage: tools/determinism-check.sh [runs] [--load]     (default 5 runs)
#        --load saturates every core first. USE IT.
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
#
# ARGUMENT PARSING IS PART OF THE INSTRUMENT.
#
# This used to be `N="${1:-5}"` with --load only recognised in position 2. Invoked
# the obvious way -- `determinism-check.sh --load` -- N became the STRING "--load",
# `seq 1 --load` errored, THE RUN LOOP EXECUTED ZERO TIMES for every test, and the
# tool printed:
#
#     imxrt1180-pwm            PASS=0
#     ...
#     All tests deterministic over --load runs.
#
# Zero runs have zero variance, so everything was "deterministic". A CHECK THAT DID
# NOT RUN LOOKS EXACTLY LIKE A CHECK THAT FOUND NOTHING -- in the tool built to
# catch precisely that, and found only because a run under load printed PASS=0
# instead of PASS=5 and I read the number instead of the last line.
# (ollama_95_neutron, 2026-07-12: "the tools are not the discipline; the tools are
# where the discipline goes to hide.")
#
# So now: flags are position-free, N MUST be a positive integer or we die, and the
# loop PROVES IT RAN before any verdict is printed.
#
N=5
LOAD=0
for a in "$@"; do
    case "$a" in
        --load) LOAD=1 ;;
        ''|*[!0-9]*) echo "usage: $0 [N] [--load]   (got '$a')" >&2; exit 2 ;;
        *)      N="$a" ;;
    esac
done
[ "$N" -ge 1 ] 2>/dev/null || { echo "N must be >= 1 (got '$N')" >&2; exit 2; }
flaky=0
load_pids=""
if [ "$LOAD" = 1 ]; then
    echo "saturating $(nproc) cores -- a green suite on an idle box measures the box."
    for _ in $(seq "$(nproc)"); do yes > /dev/null & load_pids="$load_pids $!"; done
    trap 'kill $load_pids 2>/dev/null' EXIT
fi

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
    #
    # THE LOOP MUST PROVE IT RAN. Without this, any future breakage that empties
    # the loop (a bad N, a `seq` that errors, a `continue` added above) reports
    # PASS=0 and is scored as "not flaky" -- silence read as agreement.
    #
    if [ $((pass + fail)) -ne "$N" ]; then
        printf "  %-24s *** RAN %d/%d TIMES -- THE HARNESS IS BROKEN, NOT THE TEST ***\n" \
               "$t" "$((pass + fail))" "$N"
        flaky=$((flaky+1)); continue
    fi
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
