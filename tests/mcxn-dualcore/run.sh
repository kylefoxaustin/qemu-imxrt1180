#!/usr/bin/env bash
# MCXN947 dual-M33 release smoke test.
# Builds a tiny firmware where cpu0 releases cpu1 via SYSCON, runs it on the
# frdm-mcxn947 machine, and asserts BOTH cores' banners appear on the LPUART.
#
# Overridable: QEMU=<qemu-system-arm>  CC=<arm-none-eabi-gcc>
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/dualcore.elf"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "SKIP: $CC not found"; exit 0
fi
if [ ! -x "$QEMU" ]; then
    echo "SKIP: qemu not built at $QEMU"; exit 0
fi

"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(timeout -k 5 10 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>&1 || true)"

echo "--- guest output ---"
echo "$OUT"
echo "--------------------"

fail=0
echo "$OUT" | grep -q "CPU0 up" || { echo "FAIL: cpu0 banner missing"; fail=1; }
echo "$OUT" | grep -q "CPU1 up" || { echo "FAIL: cpu1 banner missing (cpu0 did not release cpu1)"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: cpu0 booted and released cpu1 (both banners seen)"
exit $fail
