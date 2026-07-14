#!/usr/bin/env bash
# Peripheral-triggered eDMA over a real LPUART2 wire, with an echo peer.
#
# Same harness rules the uartlink test paid for:
#   - EPHEMERAL PORT, not a fixed one: a fixed port is a race with any other run
#     on the box, and the loser reports a MODEL failure for a SOCKET problem.
#   - WAIT FOR THE RUN TO FINISH; do not budget wall-clock time. The firmware
#     semihosting-exits the moment it has a verdict. The outer `timeout` is a
#     DEADLOCK GUARD, not a timing tolerance -- a hang is reported as a hang.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
QEMU=${QEMU:-../../build/qemu-system-arm}
PORT=${PORT:-$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')}
HANG_GUARD=${HANG_GUARD:-180}
DIR="$(cd "$(dirname "$0")" && pwd)"
make -C "$DIR" dmareq.elf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=$(mktemp)

timeout -k 5 "$HANG_GUARD" "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
  -kernel "$DIR/dmareq.elf" -semihosting-config enable=on,target=native \
  -chardev socket,id=ul,host=127.0.0.1,port=$PORT,server=on,wait=off \
  -serial null -serial chardev:ul >"$OUT" 2>&1 &
QPID=$!

python3 "$DIR/echo_peer.py" "$PORT" >/dev/null 2>&1 &
PEER=$!

wait $QPID
rc=$?
kill $PEER >/dev/null 2>&1

if grep -qE 'DMAREQ:' "$OUT"; then
    grep -E 'DMAREQ:' "$OUT"
    grep -q 'DMAREQ: PASS' "$OUT" || { rm -f "$OUT"; exit 1; }   # FAIL wins
elif [ $rc -eq 124 ]; then
    echo "DMAREQ: FAIL - hung (no verdict within ${HANG_GUARD}s deadlock guard)"
    rm -f "$OUT"; exit 1
else
    echo "DMAREQ: FAIL - no verdict (qemu exited rc=$rc)"
    rm -f "$OUT"; exit 1
fi
rm -f "$OUT"
