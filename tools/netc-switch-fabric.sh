#!/usr/bin/env bash
#
# RT1180 as the L2 SWITCH FABRIC of the fleet's cross-silicon segment.
#
# Instead of every node on one shared mcast hub, RT1180's NETC switch (SW0) sits in
# the MIDDLE: each peer attaches to its own physical WIRE PORT over a point-to-point
# udp socket, and the switch learns MACs + floods/forwards between the ports.  Point-
# to-point (not a shared mcast group) is REQUIRED: QEMU forces IP_MULTICAST_LOOP, so
# a switch flooding onto a shared group re-ingests its own flood and storms.
#
# WIRE-PORT MAP (host 127.0.0.1).  udp mode is symmetric: each end BINDS its localaddr
# and SENDS to the peer's localaddr -- there is no TCP-style listen/connect.  For each
# link below, the PEER uses the mirror:  socket,udp=<our localaddr>,localaddr=<our udp>.
#
#   wire port 0  (default -nic)  our: udp=127.0.0.1:45001 localaddr=127.0.0.1:45002
#   wire port 1  (netc-port1)    our: udp=127.0.0.1:45011 localaddr=127.0.0.1:45012   <- mcx  0x88B5
#   wire port 2  (netc-port2)    our: udp=127.0.0.1:45021 localaddr=127.0.0.1:45022   <- imx95 0x88B7
#
# So a peer on wire port 2 (imx95) launches its ENETC with:
#     -netdev socket,udp=127.0.0.1:45022,localaddr=127.0.0.1:45021,id=<peer-id>
#   (binds 45021 = our switch's send-dest; sends to 45022 = our switch's bind)
# and on wire port 1 (mcx):
#     -netdev socket,udp=127.0.0.1:45012,localaddr=127.0.0.1:45011,id=<peer-id>
#
# The switch learns each peer's src MAC on ingress, floods a broadcast/unknown-unicast
# out the OTHER wire ports (split-horizon off the ingress), and unicasts a known dst to
# the single port its FDB resolved -- so beacon discovery propagates and, once learned,
# traffic is directed.
#
# Modes:
#   tools/netc-switch-fabric.sh              self-test: prove the fabric floods from
#                                            EVERY wire port to the other two (host-driven).
#   tools/netc-switch-fabric.sh --host       bring the switch up on the ports above and
#                                            hold it for real peers to attach.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
case "$QEMU" in /*) ;; *) QEMU="$PWD/$QEMU" ;; esac
MODE="${1:-selftest}"

FW="$ROOT/tests/imxrt1180-netc-flood/flood.elf"   # switch firmware: enable RX ring -> forward
make -C "$ROOT/tests/imxrt1180-netc-flood" flood.elf QEMU="$QEMU" >/dev/null 2>&1 \
    || { echo "BUILD FAILED (switch firmware)"; exit 1; }

for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done

WIRING=(
  -nic    "socket,udp=127.0.0.1:45001,localaddr=127.0.0.1:45002"
  -netdev "socket,udp=127.0.0.1:45011,localaddr=127.0.0.1:45012,id=netc-port1"
  -netdev "socket,udp=127.0.0.1:45021,localaddr=127.0.0.1:45022,id=netc-port2"
  -netdev "socket,udp=127.0.0.1:45031,localaddr=127.0.0.1:45032,id=netc-port3"
)

if [ "$MODE" = "--host" ]; then
    echo ">> RT1180 switch fabric UP. Wire ports (peers attach with the MIRROR udp/localaddr):"
    echo "   port 1 (mcx  0x88B5): peer -netdev socket,udp=127.0.0.1:45012,localaddr=127.0.0.1:45011,id=<id>"
    echo "   port 2 (imx95 0x88B7): peer -netdev socket,udp=127.0.0.1:45022,localaddr=127.0.0.1:45021,id=<id>"
    echo "   port 3 (imx93 0x88B9): peer -netdev socket,udp=127.0.0.1:45032,localaddr=127.0.0.1:45031,id=<id>"
    echo "   port 0 (spare 0x88B8): peer -nic    socket,udp=127.0.0.1:45002,localaddr=127.0.0.1:45001"
    echo ">> Ctrl-C to stop."
    exec "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
        -kernel "$FW" -semihosting-config enable=on,target=native "${WIRING[@]}" -serial stdio
fi

# ---- self-test: our switch floods from EVERY wire port to the others ---------------
echo ">> self-test: RT1180 switch floods a beacon from each of ports 0/1/2/3 to the others"
"$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
    -kernel "$FW" -semihosting-config enable=on,target=native "${WIRING[@]}" \
    -serial null </dev/null >/tmp/fabric.qemu 2>&1 &
QPID=$!
sleep 2

python3 - <<'PY'
import socket, struct, sys, time
HOST="127.0.0.1"
# (our-localaddr Q, our-send-dest PY) per wire port -- we bind PY and send to Q.
PORTS = {0:(45001,45002), 1:(45011,45012), 2:(45021,45022), 3:(45031,45032)}
SRC   = {0:bytes.fromhex("020000000000"), 1:bytes.fromhex("02000000000b"),
         2:bytes.fromhex("020000000007"), 3:bytes.fromhex("020000000093")}
BCAST=b"\xff"*6; ET=0x88C2; BODY=b"FABRIC-FLOOD"
def frame(src): return BCAST+src+struct.pack(">H",ET)+BODY
sk={}
for p,(py,q) in PORTS.items():
    s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM,socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind((HOST,py)); s.setblocking(False); sk[p]=s
def drain(p):
    got=set()
    try:
        while True:
            d,_=sk[p].recvfrom(2048)
            if len(d)>=14 and d[0:6]==BCAST and struct.unpack(">H",d[12:14])[0]==ET and d[14:]==BODY:
                got.add(d[6:12])
    except BlockingIOError: pass
    return got
ports=sorted(PORTS)
ok=True
for ing in ports:
    others=[p for p in ports if p!=ing]
    seen={p:False for p in others}
    dl=time.time()+8
    while time.time()<dl and not all(seen.values()):
        sk[ing].sendto(frame(SRC[ing]),(HOST,PORTS[ing][1]))  # inject on ingress port
        time.sleep(0.12)
        for p in others:
            if SRC[ing] in drain(p): seen[p]=True
    res=all(seen.values())
    print("  ingress port %d -> flood to %s : %s" % (ing, others, "OK" if res else ("MISSING "+str([p for p in others if not seen[p]]))))
    ok = ok and res
print("FABRIC-SELFTEST: PASS - the RT1180 switch floods from every wire port to all the others"
      if ok else "FABRIC-SELFTEST: FAIL")
sys.exit(0 if ok else 1)
PY
rc=$?
kill -9 "$QPID" 2>/dev/null
for p in $(pidof qemu-system-arm 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
exit "$rc"
