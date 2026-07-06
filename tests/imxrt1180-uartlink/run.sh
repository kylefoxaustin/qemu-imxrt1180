#!/usr/bin/env bash
# Boot the RT1180 with LPUART2 on a socket, echo via a Python peer, check PASS.
set -u
QEMU=${QEMU:-../../build/qemu-system-arm}
PORT=${PORT:-14780}
DIR="$(cd "$(dirname "$0")" && pwd)"
make -C "$DIR" uartlink.elf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=$(mktemp)
# QEMU is the socket server (LPUART2 = serial_hd(1)); console via semihosting.
"$QEMU" -M mimxrt1180-evk -display none -monitor none \
  -kernel "$DIR/uartlink.elf" -semihosting-config enable=on,target=native \
  -chardev socket,id=ul,host=127.0.0.1,port=$PORT,server=on,wait=off \
  -serial null -serial chardev:ul >"$OUT" 2>&1 &
QPID=$!
sleep 0.5
python3 "$DIR/uart_peer.py" "$PORT" &
PPID=$!
for _ in $(seq 1 40); do grep -qE 'UARTLINK:' "$OUT" && break; sleep 0.25; done
grep -E 'UARTLINK:' "$OUT" || echo "UARTLINK: FAIL - no result (timeout)"
kill $QPID $PPID >/dev/null 2>&1
rm -f "$OUT"
