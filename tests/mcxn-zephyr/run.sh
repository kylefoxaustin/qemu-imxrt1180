#!/usr/bin/env bash
# Boot real Zephyr samples for frdm_mcxn947 on the model and assert behaviour.
# CI-safe: each check skips when its ELF is not available.
#
# Build the ELFs in a Zephyr workspace, e.g.:
#   west build -b frdm_mcxn947/mcxn947/cpu0 samples/hello_world
#   west build -b frdm_mcxn947/mcxn947/cpu0 samples/basic/blinky -d build_blinky
# Override paths with ZEPHYR_ELF= and BLINKY_ELF=.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
ZDIR="$HOME/zephyrproject/zephyr"
ZEPHYR_ELF="${ZEPHYR_ELF:-$ZDIR/build/zephyr/zephyr.elf}"
BLINKY_ELF="${BLINKY_ELF:-$ZDIR/build_blinky/zephyr/zephyr.elf}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

boot() { timeout -k 5 12 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
                 -serial stdio -kernel "$1" -no-reboot 2>/dev/null || true; }

fail=0; ran=0

# hello_world: boot banner reaches the console
if [ -f "$ZEPHYR_ELF" ]; then
    ran=1; OUT="$(boot "$ZEPHYR_ELF")"
    echo "$OUT" | grep -q "Booting Zephyr OS" || { echo "FAIL: no Zephyr boot banner"; fail=1; }
    echo "$OUT" | grep -q "Hello World"      || { echo "FAIL: no Hello World"; fail=1; }
    echo "hello_world: $(echo "$OUT" | grep -c 'Hello World') banner line(s)"
fi

# blinky: the LED GPIO actually toggles (both ON and OFF states observed)
if [ -f "$BLINKY_ELF" ]; then
    ran=1; OUT="$(boot "$BLINKY_ELF")"
    echo "$OUT" | grep -q "LED state: ON"  || { echo "FAIL: blinky never set LED ON"; fail=1; }
    echo "$OUT" | grep -q "LED state: OFF" || { echo "FAIL: blinky never set LED OFF"; fail=1; }
    echo "blinky: $(echo "$OUT" | grep -c 'LED state') toggle(s) on gpio0"
fi

[ "$ran" = 1 ] || { echo "SKIP: no Zephyr ELFs found"; exit 0; }
[ $fail -eq 0 ] && echo "PASS: Zephyr boots and drives GPIO on frdm-mcxn947"
exit $fail
