#!/usr/bin/env bash
#
# NETC switch WIRE->WIRE routing test: a frame ingressing one physical wire port is
# routed out ANOTHER physical wire port (multi-physical-port). Port 0 uses the default
# -nic; ports 1 and 2 attach to their own mcast-socket netdevs (netc-port1/2), so the
# switch routes between three independent wires. portfwd.py drives + verifies host-side.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
case "$QEMU" in /*) ;; *) QEMU="$PWD/$QEMU" ;; esac

for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
make -C "$HERE" portfwd.elf QEMU="$QEMU" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

# Wire ports 1 and 2 are POINT-TO-POINT udp links (no shared-mcast self-loopback:
# see portfwd.py).  Port 0 and 3 stay unplugged (-nic none), out of the routing.
#   port 1: QEMU recv on :44012, sends egress to our :44011
#   port 2: QEMU recv on :44022, sends egress to our :44021
"$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
    -kernel "$HERE/portfwd.elf" -semihosting-config enable=on,target=native \
    -nic none \
    -netdev socket,udp=127.0.0.1:44011,localaddr=127.0.0.1:44012,id=netc-port1 \
    -netdev socket,udp=127.0.0.1:44021,localaddr=127.0.0.1:44022,id=netc-port2 \
    -serial null </dev/null >/tmp/portfwd.qemu 2>&1 &
QPID=$!
sleep 2                                   # let the guest enable the RX ring

python3 "$HERE/portfwd.py"; rc=$?

kill -9 "$QPID" 2>/dev/null
for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
exit "$rc"
