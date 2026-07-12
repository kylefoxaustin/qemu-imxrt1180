#!/usr/bin/env bash
# Boot the RT1180 with LPUART2 on a socket, echo via a Python peer, check PASS.
#
# THE HARNESS IS AN INSTRUMENT TOO.  This used to poll for a result for a fixed
# 10 s of WALL-CLOCK time (40 x 0.25 s) and then declare
#     "UARTLINK: FAIL - no result (timeout)"
# -- so under heavy host load, when QEMU and the Python peer simply take longer,
# it reported a FAILURE THAT WAS A MEASUREMENT OF THE BOX, not of the model.
# Found by running the suite under saturation: 2/3 under load, 3/3 idle.
#
# (mcxn947qemu, 2026-07-12, whose ztest "regression" was the same thing: "The
# failure was the HARNESS, not the model. I fixed the instrument I was looking at
# and never audited the instrument I was STANDING ON. Fix your CI, not just your
# tests.")
#
# Now: WAIT FOR THE RUN TO FINISH; do not guess how long it should take. The
# firmware semihosting-exits the moment it has a verdict, so waiting on the
# process is exact rather than budgeted.  The outer `timeout` is a DEADLOCK GUARD,
# not a timing tolerance -- it is generous enough that only a genuine hang trips
# it, and a hang is reported as a hang rather than as a failed comparison.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
QEMU=${QEMU:-../../build/qemu-system-arm}
# A FIXED PORT IS A RACE. Back-to-back runs (and any other test or stray QEMU on
# the box) contend for it, and the loser reports a MODEL failure for a SOCKET
# problem. Pick a free ephemeral port per run instead of hoping 14780 is idle.
PORT=${PORT:-$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')}
HANG_GUARD=${HANG_GUARD:-120}      # deadlock guard, NOT a timing tolerance
DIR="$(cd "$(dirname "$0")" && pwd)"
make -C "$DIR" uartlink.elf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=$(mktemp)

# QEMU is the socket server (LPUART2 = serial_hd(1)); console via semihosting.
timeout "$HANG_GUARD" "$QEMU" -M mimxrt1180-evk -display none -monitor none \
  -kernel "$DIR/uartlink.elf" -semihosting-config enable=on,target=native \
  -chardev socket,id=ul,host=127.0.0.1,port=$PORT,server=on,wait=off \
  -serial null -serial chardev:ul >"$OUT" 2>&1 &
QPID=$!

python3 "$DIR/uart_peer.py" "$PORT" >/dev/null 2>&1 &
PEER=$!

wait $QPID                          # firmware exits by itself once it has a verdict
rc=$?
kill $PEER >/dev/null 2>&1

if grep -qE 'UARTLINK:' "$OUT"; then
    grep -E 'UARTLINK:' "$OUT"
elif [ $rc -eq 124 ]; then
    echo "UARTLINK: FAIL - hung (no verdict within ${HANG_GUARD}s deadlock guard)"
else
    echo "UARTLINK: FAIL - no verdict (qemu exited rc=$rc)"
fi
rm -f "$OUT"
