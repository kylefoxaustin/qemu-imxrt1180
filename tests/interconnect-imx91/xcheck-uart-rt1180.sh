#!/usr/bin/env bash
#
# Cross-check the RT1180 half of the uart-link-91-rt1180 holobench cell.
#
# Boots the RT1180 QEMU with the persistent passthrough firmware on LPUART2
# (serial_hd(1)) as a socket SERVER, then runs uart_driver.py (the i.MX91-side
# stand-in) as the client to verify a byte-exact echo.  Both sides use the raw
# GO+echo protocol the real i.MX91 <-> RT1180 link uses; the RT1180 side stays
# alive (no SYS_EXIT) exactly as holobench needs.
#
# SKIPs cleanly (exit 0) if the RT1180 QEMU or passthrough.elf isn't built, so
# it can live in the coordinator repo and run anywhere.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$DIR/../.."
QEMU=${QEMU:-$ROOT/build/qemu-system-arm}
ELF="$ROOT/tests/imxrt1180-uartlink/passthrough.elf"
PORT=${PORT:-15791}

if [ ! -x "$QEMU" ] || [ ! -f "$ELF" ]; then
    echo "SKIP: RT1180 qemu-system-arm or passthrough.elf not built"
    exit 0
fi

OUT=$(mktemp)
# RT1180 = socket server; console banner via semihosting -> the log.
"$QEMU" -M mimxrt1180-evk -display none -monitor none \
    -kernel "$ELF" -semihosting-config enable=on,target=native \
    -chardev socket,id=ul,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -serial null -serial chardev:ul >"$OUT" 2>&1 &
QPID=$!
# Wait for the RT1180 to announce it's ready.
for _ in $(seq 1 40); do grep -qi 'PASSTHROUGH ready' "$OUT" && break; sleep 0.25; done

python3 "$DIR/uart_driver.py" "$PORT"
rc=$?

# The RT1180 must still be alive after the exchange (persistent for the farm).
if kill -0 "$QPID" 2>/dev/null; then
    echo "RT1180: still alive after the exchange (persistent, holobench-hostable)"
else
    echo "RT1180: FAIL - process exited (should stay alive for QMP)"
    rc=1
fi
kill "$QPID" 2>/dev/null
rm -f "$OUT"
exit $rc
