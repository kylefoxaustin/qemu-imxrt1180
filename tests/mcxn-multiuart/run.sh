#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"; CC="${CC:-arm-none-eabi-gcc}"; ELF="$HERE/multiuart.elf"
command -v "$CC" >/dev/null 2>&1 || { echo SKIP; exit 0; }; [ -x "$QEMU" ] || { echo SKIP; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
F0=$(mktemp); F1=$(mktemp)
timeout -k 5 8 "$QEMU" -M frdm-mcxn947 -display none -serial "file:$F0" -serial "file:$F1" -kernel "$ELF" -no-reboot >/dev/null 2>&1 || true
echo "serial0 (FlexComm4): $(cat $F0)"; echo "serial1 (FlexComm2): $(cat $F1)"
ok=1; grep -q "FC4 hello" "$F0" || { echo "FAIL: FC4 not on serial0"; ok=0; }
grep -q "FC2 hello" "$F1" || { echo "FAIL: FC2 not on serial1"; ok=0; }
rm -f "$F0" "$F1"; [ $ok = 1 ] && { echo "PASS: independent FlexComm UARTs"; exit 0; } || exit 1
