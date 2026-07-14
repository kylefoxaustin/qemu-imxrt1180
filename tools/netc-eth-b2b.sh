#!/usr/bin/env bash
#
# 1180<->1180 L2 Ethernet board-to-board test.
#
# Builds the SDK netc_txrx_transfer example with PHY local-loopback DISABLED (so
# TX frames egress to the wire instead of U-turning), then runs two mimxrt1180-evk
# QEMU instances on a shared `-nic socket` L2 segment.  Each instance broadcasts
# 20 frames; the test confirms each RECEIVES the other's frames byte-exact,
# proving the NETC (ENETC) is a real Ethernet endpoint over a virtual wire.
#
# SETUP: same as tools/sdk-run.sh (extracted SDK + python venv + arm-none-eabi-gcc).
# USAGE: SDK_ROOT=/path/to/sdk/mcuxsdk VENV=/path/to/venv tools/netc-eth-b2b.sh
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
SDK_ROOT="${SDK_ROOT:?set SDK_ROOT to the extracted .../sdk/mcuxsdk}"
VENV="${VENV:?set VENV to the python venv with west+kconfiglib}"
export ARMGCC_DIR="${ARMGCC_DIR:-/usr}" PATH="$VENV/bin:$PATH"
PORT="${PORT:-12471}"
EX=examples/driver_examples/netc/txrx_transfer
HW="$SDK_ROOT/examples/_boards/evkmimxrt1180/driver_examples/netc/txrx_transfer/cm33/hardware_init.c"
LB='return PHY_EnableLoopback(&s_phy_handle[port], kPHY_LocalLoop, phyConfig->speed, true);'
NLB='return kStatus_Success; /* b2b: no loopback, egress to the wire */'
BUILD=/tmp/netc_nolb

echo ">> building non-loopback netc firmware"
sed -i "s|$LB|$NLB|" "$HW"
( cd "$SDK_ROOT" && west build -b evkmimxrt1180 --toolchain armgcc "$EX" \
    -Dcore_id=cm33 --config debug -d "$BUILD" ) >/tmp/netc_nolb_build.log 2>&1
rc=$?
sed -i "s|$NLB|$LB|" "$HW"            # restore the source
[ $rc -eq 0 ] || { echo "   BUILD FAILED"; tail -3 /tmp/netc_nolb_build.log; exit 1; }
ELF="$(ls "$BUILD"/*.elf | head -1)"

echo ">> launching two instances on a socket L2 segment (port $PORT)"
C="-M mimxrt1180-evk -audio none -display none -monitor none -kernel $ELF -semihosting-config enable=on,target=native"
timeout -k 5 14 $QEMU $C -nic socket,listen=127.0.0.1:$PORT -serial file:/tmp/ethA.con >/dev/null 2>&1 &
A=$!
sleep 0.6
timeout -k 5 14 $QEMU $C -nic socket,connect=127.0.0.1:$PORT -serial file:/tmp/ethB.con >/dev/null 2>&1 &
B=$!
wait $A 2>/dev/null; wait $B 2>/dev/null

for i in A B; do
    f=/tmp/eth$i.con
    tx=$(grep -c 'transmitted success' "$f" 2>/dev/null)
    rx=$(grep -c 'A frame received' "$f" 2>/dev/null)
    mm=$(grep -c "don't match" "$f" 2>/dev/null)
    echo "instance $i: TX=$tx RX=$rx mismatch=$mm"
done
echo "PASS if both RX>0 and mismatch=0 (each received the other's frames byte-exact)."
