#!/usr/bin/env bash
# MCXN947 PowerQuad coprocessor (CP0) scalar-math test: drives sin/cos/sqrt/inv/
# ln/exp via MCR/MRC and division via MCRR, asserting exact float32 results.
# On stock QEMU (no PowerQuad coprocessor) these would NOCP->HardFault.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/pqcp.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 10 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "PQCP PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
