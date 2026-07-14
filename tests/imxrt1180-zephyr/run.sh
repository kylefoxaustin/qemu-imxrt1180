#!/bin/bash
# Zephyr saturation harness: boot a corpus of real Zephyr RTOS images on the
# mimxrt1180-evk machine and report pass / run / fault.  Exercises the model
# against a breadth of live RTOS firmware (scheduler, IPC, timers, FPU, C++...).
#
# Build the corpus first (needs a Zephyr workspace + arm-none-eabi-gcc), e.g.:
#   cd ~/zephyrproject && source .venv/bin/activate
#   export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb GNUARMEMB_TOOLCHAIN_PATH=/usr
#   B=mimxrt1180_evk/mimxrt1189/cm33
#   for s in hello_world synchronization philosophers \
#            kernel/condition_variables/simple cpp/cpp_synchronization; do
#     west build -p always -b $B zephyr/samples/$s -d /tmp/zcorpus/$(basename $s)
#   done
# then:  ZEPHYR_BUILDS=/tmp/zcorpus tests/imxrt1180-zephyr/run.sh
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
BUILDS="${ZEPHYR_BUILDS:?set ZEPHYR_BUILDS to a dir of 'west build' output dirs}"
TIMEOUT="${TIMEOUT:-12}"

printf "%-26s %-9s %s\n" "ZEPHYR IMAGE" "RESULT" "EVIDENCE"
printf "%-26s %-9s %s\n" "------------" "------" "--------"
for d in "$BUILDS"/*/; do
    elf="$d/zephyr/zephyr.elf"
    [ -f "$elf" ] || continue
    name=$(basename "$d")
    out=$(timeout "$TIMEOUT" "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
        -kernel "$elf" -serial stdio -semihosting-config enable=on,target=native \
        </dev/null 2>/tmp/z_err.$$)
    if grep -qiE "PROJECT EXECUTION SUCCESSFUL" <<<"$out"; then
        res="PASS"; ev="ztest SUCCESSFUL"
    elif grep -qiE "PROJECT EXECUTION FAILED|] FAIL " <<<"$out"; then
        res="TESTFAIL"; ev=$(grep -iE "FAIL" <<<"$out" | head -1 | cut -c1-42)
    elif grep -qiE "Lockup|fatal|assert" /tmp/z_err.$$; then
        res="FAULT"; ev=$(grep -iE "Lockup|fatal" /tmp/z_err.$$ | head -1 | cut -c1-42)
    elif grep -qiE "Booting Zephyr" <<<"$out"; then
        res="BOOT/RUN"
        ev=$(grep -avE "Booting Zephyr|^\s*$" <<<"$out" | tr -d '\033' | \
             sed -E 's/\[[0-9;]*[A-Za-z]//g' | grep -aE '[A-Za-z]' | head -1 | cut -c1-42)
    else
        res="NOOUT"; ev=""
    fi
    printf "%-26s %-9s [%s]\n" "$name" "$res" "$ev"
    rm -f /tmp/z_err.$$
done
