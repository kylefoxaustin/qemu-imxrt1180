#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"; CC="${CC:-arm-none-eabi-gcc}"; ELF="$HERE/dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo SKIP; exit 0; }; [ -x "$QEMU" ] || { echo SKIP; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 10 "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "$OUT"; echo "$OUT" | grep -q "DMA PASS" && { echo PASS; exit 0; } || { echo FAIL; exit 1; }
