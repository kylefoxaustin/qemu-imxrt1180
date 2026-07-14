#!/usr/bin/env bash
#
# RT1180 NETC "eth-lab" node — model-agnostic raw-L2 cross-check (MCX-compatible).
#
# Transforms the SDK netc_txrx_transfer example into an eth-lab node that
# periodically BROADCASTS an experimental-ethertype (0x88B5) frame and prints
# each RECEIVED peer frame's ethertype + source MAC -- the same shape as the MCX
# tests/mcxn-enet-lab node, so an RT1180 and an MCX (or i.MX) interoperate over
# one `-nic socket` with zero IP-stack agreement.
#
# With no args it runs an RT1180<->RT1180 self-check.  Point $PEER at an external
# socket (host:port) to join a cross-model segment instead (e.g. MCX's listener).
#
# SETUP: same as tools/sdk-run.sh.  USAGE: SDK_ROOT=... VENV=... tools/netc-eth-lab.sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
SDK_ROOT="${SDK_ROOT:?set SDK_ROOT}"; VENV="${VENV:?set VENV}"
export ARMGCC_DIR="${ARMGCC_DIR:-/usr}" PATH="$VENV/bin:$PATH"
PORT="${PORT:-12491}"
EXDIR="$SDK_ROOT/examples/driver_examples/netc/txrx_transfer"
SRC="$EXDIR/netc_txrx_transfer.c"
HW="$SDK_ROOT/examples/_boards/evkmimxrt1180/driver_examples/netc/txrx_transfer/cm33/hardware_init.c"
BUILD=/tmp/netc_lab

echo ">> patching netc example into an eth-lab node"
cp "$SRC" "$SRC.orig"; cp "$HW" "$HW.orig"
python3 - "$SRC" "$HW" <<'PY'
import sys
src, hw = sys.argv[1], sys.argv[2]
t = open(src).read()
# 1) ethertype 0x88B5 in the broadcast frame
t = t.replace("    g_txFrame[12] = (length >> 8U) & 0xFFU;\n    g_txFrame[13] = length & 0xFFU;",
              "    g_txFrame[12] = 0x88U; /* eth-lab EtherType 0x88B5 */\n    g_txFrame[13] = 0xB5U;")
# 2) replace the strict send/recv/memcmp loop with a bounded broadcast + non-blocking-RX + print loop
import re
start = t.index("    while (txFrameNum < EXAMPLE_EP_TXFRAME_NUM)")
end   = t.index("\n    }\n", start) + len("\n    }\n")
loop = '''    PRINTF("ENET-LAB up: RT1180 broadcasting ethertype 0x88B5\\r\\n");
    for (uint32_t it = 0; it < 60U; it++)
    {
        txOver = false;
        if (EP_SendFrame(&g_ep_handle, 0, &txFrame, NULL, NULL) == kStatus_Success)
        {
            while (!txOver) { }
            EP_ReclaimTxDescriptor(&g_ep_handle, 0);
        }
        if (EP_GetRxFrameSize(&g_ep_handle, 0, &length) == kStatus_Success)
        {
            (void)EP_ReceiveFrameCopy(&g_ep_handle, 0, g_rxFrame, length, NULL);
            PRINTF("ENET-LAB rx: ethertype 0x%02x%02x src %02x:%02x:%02x:%02x:%02x:%02x\\r\\n",
                   g_rxFrame[12], g_rxFrame[13], g_rxFrame[6], g_rxFrame[7],
                   g_rxFrame[8], g_rxFrame[9], g_rxFrame[10], g_rxFrame[11]);
        }
        for (volatile uint32_t dly = 0; dly < 1500000U; dly++) { }
    }
'''
t = t[:start] + loop + t[end:]
open(src, "w").write(t)
# 3) disable PHY local loopback so frames egress to the wire
h = open(hw).read().replace(
    "return PHY_EnableLoopback(&s_phy_handle[port], kPHY_LocalLoop, phyConfig->speed, true);",
    "return kStatus_Success; /* eth-lab: no loopback, egress to the wire */")
open(hw, "w").write(h)
print("patched")
PY

echo ">> building"
( cd "$SDK_ROOT" && west build -b evkmimxrt1180 --toolchain armgcc \
    examples/driver_examples/netc/txrx_transfer -Dcore_id=cm33 --config debug \
    -d "$BUILD" ) >/tmp/netc_lab_build.log 2>&1
rc=$?
mv "$SRC.orig" "$SRC"; mv "$HW.orig" "$HW"          # restore pristine sources
[ $rc -eq 0 ] || { echo "BUILD FAILED"; tail -3 /tmp/netc_lab_build.log; exit 1; }
ELF="$(ls "$BUILD"/*.elf | head -1)"
C="-M mimxrt1180-evk -audio none -display none -monitor none -kernel $ELF -semihosting-config enable=on,target=native"

if [ -n "${PEER:-}" ]; then
    echo ">> joining external segment at $PEER (cross-model)"
    timeout -k 5 12 $QEMU $C -nic socket,connect=$PEER,mac=02:52:8d:11:11:aa -serial file:/tmp/labX.con >/dev/null 2>&1
    echo "0x88B5 rx=$(grep -ac 'ENET-LAB rx: ethertype 0x88b5' /tmp/labX.con || echo 0)"
    grep -m3 'ENET-LAB rx' /tmp/labX.con
    exit 0
fi

echo ">> RT1180<->RT1180 self-check on port $PORT"
pkill -9 -f qemu-system-arm 2>/dev/null; sleep 1
timeout -k 5 9 $QEMU $C -nic socket,listen=127.0.0.1:$PORT,mac=02:52:8d:11:11:01 -serial file:/tmp/labA.con >/dev/null 2>&1 &
sleep 1
timeout -k 5 9 $QEMU $C -nic socket,connect=127.0.0.1:$PORT,mac=02:52:8d:11:11:02 -serial file:/tmp/labB.con >/dev/null 2>&1 &
wait
A=$(grep -ac 'ENET-LAB rx: ethertype 0x88b5' /tmp/labA.con 2>/dev/null); A=${A:-0}
B=$(grep -ac 'ENET-LAB rx: ethertype 0x88b5' /tmp/labB.con 2>/dev/null); B=${B:-0}
echo "node A 0x88B5 rx=$A | node B 0x88B5 rx=$B"
{ [ "$A" -ge 1 ] && [ "$B" -ge 1 ]; } && echo "PASS: raw-L2 0x88B5 both ways" || echo "FAIL"
