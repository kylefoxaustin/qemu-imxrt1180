#!/usr/bin/env bash
#
# NETC switch BROADCAST-FLOOD test: a broadcast ingressing one physical wire port is
# flooded out every OTHER physical wire port.  Three wire ports are wired
# point-to-point to the host (port 0 = default -nic; ports 1,2 = netc-port1/2), so
# the switch floods between three independent wires.  flood.py drives + verifies.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
case "$QEMU" in /*) ;; *) QEMU="$PWD/$QEMU" ;; esac

for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
make -C "$HERE" flood.elf QEMU="$QEMU" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

# Three POINT-TO-POINT udp wires (no shared-mcast self-loopback: see flood.py).
#   port 0: QEMU recv on :44002, sends egress to our :44001
#   port 1: QEMU recv on :44012, sends egress to our :44011   (injection wire)
#   port 2: QEMU recv on :44022, sends egress to our :44021
"$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
    -kernel "$HERE/flood.elf" -semihosting-config enable=on,target=native \
    -nic socket,udp=127.0.0.1:44001,localaddr=127.0.0.1:44002 \
    -netdev socket,udp=127.0.0.1:44011,localaddr=127.0.0.1:44012,id=netc-port1 \
    -netdev socket,udp=127.0.0.1:44021,localaddr=127.0.0.1:44022,id=netc-port2 \
    -serial null </dev/null >/tmp/flood.qemu 2>&1 &
QPID=$!
sleep 2                                   # let the guest enable the RX ring

python3 "$HERE/flood.py"; rc=$?

kill -9 "$QPID" 2>/dev/null
for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
exit "$rc"
