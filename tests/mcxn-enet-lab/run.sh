#!/usr/bin/env bash
# Self-test for the cross-board ENET L2 lab emitter: run two MCX instances on a
# point-to-point socket link and assert each receives the peer's ethertype-0x88B5
# broadcast.  (In the real holobench mcx93-eth lab the peer is the i.MX93 FEC.)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
ELF="${LAB_ELF:-$HOME/mcxn-images/mcxn-enet-lab.elf}"
CC="${CC:-arm-none-eabi-gcc}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
if [ ! -f "$ELF" ]; then
  command -v "$CC" >/dev/null || { echo "SKIP: no lab ELF and no $CC"; exit 0; }
  ELF="$HERE/enet-lab.elf"
  "$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
    -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF" || { echo "SKIP: build failed"; exit 0; }
fi
PORT=$(( (RANDOM%20000)+20000 )); O1=$(mktemp); O2=$(mktemp)
trap 'rm -f "$O1" "$O2"' EXIT
timeout -k 5 9 "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
  -nic socket,listen=127.0.0.1:$PORT,model=mcxn-enet,mac=02:4d:43:58:00:01 -kernel "$ELF" -no-reboot >"$O1" 2>/dev/null &
sleep 1
timeout -k 5 9 "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
  -nic socket,connect=127.0.0.1:$PORT,model=mcxn-enet,mac=02:4d:43:58:00:02 -kernel "$ELF" -no-reboot >"$O2" 2>/dev/null &
wait
a=$(grep -ac 'ENET-LAB rx: ethertype 0x88b5' "$O1" || true)
b=$(grep -ac 'ENET-LAB rx: ethertype 0x88b5' "$O2" || true)
echo "node1 0x88B5 rx: $a | node2 0x88B5 rx: $b"
[ "$a" -ge 1 ] && [ "$b" -ge 1 ] && { echo "PASS: both nodes rx ethertype-0x88B5 over the wire"; exit 0; } || { echo "FAIL"; exit 1; }
