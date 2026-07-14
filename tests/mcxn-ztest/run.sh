#!/usr/bin/env bash
# Run Zephyr ztest suites built for frdm_mcxn947 on the model and assert each
# reaches "PROJECT EXECUTION SUCCESSFUL".  This validates the machine against
# Zephyr's own kernel test framework — third-party firmware, not our hand-rolled
# tests.  CI-safe: SKIPs when no staged ztest ELFs are present (they are
# operator-built; see build.sh / README.md).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
# Directory of <suite>.elf files, e.g. context.elf, fifo_api.elf, timer_behavior.elf
ZTEST_DIR="${ZTEST_DIR:-$HOME/mcxn-images/ztest}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

shopt -s nullglob
elfs=("$ZTEST_DIR"/*.elf)
[ "${#elfs[@]}" -gt 0 ] || { echo "SKIP: no ztest ELFs in $ZTEST_DIR (see build.sh)"; exit 0; }

fail=0
for elf in "${elfs[@]}"; do
    name="$(basename "$elf" .elf)"
    OUT="$(timeout -k 5 60 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
            -serial stdio -kernel "$elf" -no-reboot </dev/null 2>/dev/null || true)"
    if echo "$OUT" | grep -q "PROJECT EXECUTION SUCCESSFUL"; then
        # Count reported suite/case results for a one-line summary.
        cases="$(echo "$OUT" | grep -cE '^ (PASS|FAIL) -')"
        echo "PASS  $name ($cases cases)"
    else
        echo "FAIL  $name"
        echo "$OUT" | grep -E 'FAIL -|ASSERTION|FATAL|Fault' | head -5
        fail=1
    fi
done

[ $fail -eq 0 ] && echo "PASS: all staged Zephyr ztest suites succeeded"
exit $fail
