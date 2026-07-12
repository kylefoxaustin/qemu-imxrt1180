#!/usr/bin/env bash
#
# RT1180 EdgeLock (ELE) honesty test — does the enclave LIE TO THE GUEST?
#
# The ELE is proprietary and not modelled. A model may decline to compute. What
# it may NOT do is tell the GUEST it computed something it didn't.
#
# The SDK's ELE_* wrappers validate a reply with exactly
#     rmsg[0] == <CMD>_RESPONSE_HDR && rmsg[1] == RESPONSE_SUCCESS
# and then USE the output buffer. So a blanket SUCCESS reply is not a harmless
# stub -- it is a fabricated cryptographic result: ELE_RngGetRandom() returns
# kStatus_Success over an untouched buffer, and a crypto stack seeds with zeros
# believing it succeeded.
#
# ele.c drives the S3MU byte-for-byte as ELE_RngGetRandom() does and checks the
# two things that actually matter, from the GUEST's side:
#     - what verdict does the stock driver's own success test reach?
#     - did any randomness actually appear in the buffer?
# The only honest combination is: no randomness => driver must see failure.
#
# AND IT IS NEGATIVE-TESTED. `fake-uncomputed-success=on` restores the old
# lying behaviour; the test must then FAIL. A guard you have never seen fail is
# not a guard. (Fleet lesson, mcxn947qemu: a test written against your own model
# tests the model you wrote.)
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
rc=0

make -s -C "$HERE" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

run() {   # $1 = extra qemu args
    timeout 25 "$QEMU" -M mimxrt1180-evk -display none -monitor none $1 \
        -kernel "$HERE/ele.elf" -serial null \
        -semihosting-config enable=on,target=native 2>&1 </dev/null | tr -d '\0'
}

echo ">> honest model (default): enclave must NOT fake a crypto result"
out=$(run "")
echo "$out" | sed 's/^/   /'
if echo "$out" | grep -q "ELE-ALL: PASS"; then
    echo "   => PASS"
else
    echo "   => FAIL: the model reported SUCCESS for an un-computed enclave result"
    rc=1
fi

echo
echo ">> entropy must be UNPREDICTABLE: the same words must not come back every boot"
# 95emulator shipped a STATIC-SEEDED xorshift here: kStatus_Success, bytes that
# look perfectly random on one boot, and IDENTICAL on every boot. No single run
# can see that. Only a cross-boot diff can. (Ours was the other flavour -- an
# untouched buffer -- but the same status check passes both.)
w1=$(run "" | grep "^rng words" | head -1)
w2=$(run "" | grep "^rng words" | head -1)
echo "   boot A: $w1"
echo "   boot B: $w2"
if [ -n "$w1" ] && [ "$w1" != "$w2" ]; then
    echo "   => PASS (entropy differs across boots)"
else
    echo "   => FAIL: identical entropy on two boots -- this is a static-seeded PRNG,"
    echo "            not entropy. It will look random to any single-boot test."
    rc=1
fi

echo
echo ">> NEGATIVE TEST: force the old behaviour; the guard MUST catch it"
out=$(run "-global imxrt1180-s3mu.fake-uncomputed-success=on")
if echo "$out" | grep -q "ELE-ALL: FAIL"; then
    echo "   => PASS (guard correctly flagged the fabricated success)"
else
    echo "   => FAIL: the guard did NOT notice a lying enclave -- it cannot fail,"
    echo "            so it is proving nothing. Check the build actually changed."
    rc=1
fi

echo
[ $rc -eq 0 ] && echo "ELE: PASS (enclave is honest to the guest, and the guard can fail)" \
              || echo "ELE: FAIL"
exit $rc
