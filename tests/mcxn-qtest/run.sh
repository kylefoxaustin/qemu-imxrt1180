#!/usr/bin/env bash
# MCXN947 QOM + device-crash regression: walk the frdm-mcxn947 QOM tree and try
# instantiating arbitrary devices on the machine.  Catches realize crashes,
# reset-value/W1C issues, and MMIO access-size UB (per 93/91 playbook).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
PY="$ROOT/build/pyvenv/bin/python3"; [ -x "$PY" ] || PY=python3
rc=0
if [ -x "$ROOT/build/tests/qtest/qom-test" ]; then
  QTEST_QEMU_BINARY="$QEMU" timeout -k 5 120 "$ROOT/build/tests/qtest/qom-test" >/dev/null 2>&1 \
    && echo "qom-test: ok" || { echo "qom-test: FAIL"; rc=1; }
else echo "qom-test: SKIP (build with: ninja -C build tests/qtest/qom-test)"; fi
if [ -f "$ROOT/scripts/device-crash-test" ]; then
  PYTHONPATH="$ROOT/python" timeout -k 5 240 "$PY" "$ROOT/scripts/device-crash-test" \
    -t machine=frdm-mcxn947 -- "$QEMU" >/dev/null 2>&1 \
    && echo "device-crash-test: ok" || { echo "device-crash-test: FAIL"; rc=1; }
else echo "device-crash-test: SKIP"; fi
[ $rc -eq 0 ] && echo "PASS" || echo "FAIL"; exit $rc
