#!/usr/bin/env bash
# MCXN947 migration (savevm/loadvm) round-trip: boot the Zephyr blinky, migrate
# the full machine state to a file, restore it into a fresh instance, and assert
# the guest resumes (LED keeps toggling).  Exercises every device's VMSTATE
# serialization - a missing/broken VMSTATE field fails the save or the restore.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
BLINKY="${BLINKY_ELF:-$HOME/mcxn-images/mcxn-blinky.elf}"
MIG="$(mktemp)"; POST="$(mktemp)"
trap 'rm -f "$MIG" "$POST"' EXIT
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
[ -f "$BLINKY" ] || { echo "SKIP: no blinky ELF at $BLINKY"; exit 0; }
# Save: boot, run, migrate full state to a file.
( sleep 4; printf 'migrate "exec:cat > %s"\n' "$MIG"; sleep 3; printf 'quit\n' ) | \
  timeout -k 5 15 "$QEMU" -M frdm-mcxn947 -display none -serial null -monitor stdio \
  -kernel "$BLINKY" -no-reboot >/dev/null 2>&1 || true
[ -s "$MIG" ] || { echo "FAIL: migration file empty (save failed)"; exit 1; }
# Restore: load the state into a fresh instance, capture resumed output.
timeout -k 5 9 "$QEMU" -M frdm-mcxn947 -display none -serial "file:$POST" -monitor none \
  -incoming "exec:cat $MIG" -kernel "$BLINKY" -no-reboot >/dev/null 2>&1 || true
n=$(grep -ac 'LED state' "$POST" || true)
echo "migration file: $(stat -c%s "$MIG") bytes; resumed toggles: $n"
[ "$n" -ge 2 ] && { echo "PASS: full machine state migrated + guest resumed"; exit 0; } || { echo "FAIL: guest did not resume after restore"; exit 1; }
