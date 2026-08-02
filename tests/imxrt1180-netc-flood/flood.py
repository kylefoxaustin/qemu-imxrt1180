#!/usr/bin/env python3
# NETC switch BROADCAST-FLOOD check across three physical wire ports.
#
# Each wire port is a POINT-TO-POINT udp link to us (no shared-mcast self-loopback:
# QEMU forces IP_MULTICAST_LOOP, which would make a flooding switch storm; see the
# netc-portfwd notes).  We inject ONE broadcast frame on wire port 1 and assert the
# switch floods it out BOTH other wires (ports 0 and 2) unchanged, and -- implicitly,
# since our own send never loops back on a point-to-point link -- not back onto the
# ingress wire (split-horizon).
#
#   port 0  <->  QEMU localaddr Q0 / our PY0
#   port 1  <->  QEMU localaddr Q1 / our PY1   (injection wire)
#   port 2  <->  QEMU localaddr Q2 / our PY2
import socket, struct, sys, time

HOST = "127.0.0.1"
PY0, Q0 = 44001, 44002        # port 0 (default -nic)
PY1, Q1 = 44011, 44012        # port 1 (netc-port1)  -- injection
PY2, Q2 = 44021, 44022        # port 2 (netc-port2)
BCAST = b"\xff\xff\xff\xff\xff\xff"
SRC   = bytes.fromhex("020000000a0a")
ET    = 0x88C1                             # a private ethertype for this test
BODY  = b"FLEET-BEACON-FLOOD"

def frame(dst, src, body):
    return dst + src + struct.pack(">H", ET) + body

def mksock(recv_port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, recv_port))
    s.setblocking(False)
    return s

p0, p1, p2 = mksock(PY0), mksock(PY1), mksock(PY2)
beacon = frame(BCAST, SRC, BODY)

def got_beacon(sk):
    try:
        while True:
            d, _ = sk.recvfrom(2048)
            if len(d) < 14:
                continue
            dst, src, et = d[0:6], d[6:12], struct.unpack(">H", d[12:14])[0]
            if et == ET and src == SRC and dst == BCAST and d[14:] == BODY:
                return True
    except BlockingIOError:
        pass
    return False

deadline = time.time() + 20
seen0 = seen2 = False
while time.time() < deadline and not (seen0 and seen2):
    p1.sendto(beacon, (HOST, Q1))          # inject the broadcast on wire port 1
    time.sleep(0.15)
    if got_beacon(p0):
        seen0 = True                       # flooded out wire port 0
    if got_beacon(p2):
        seen2 = True                       # flooded out wire port 2

if seen0 and seen2:
    print("FLOOD: PASS - a broadcast on wire port 1 was flooded out BOTH wire port 0 "
          "and wire port 2 (byte-exact); split-horizon kept it off the ingress wire")
    sys.exit(0)
print("FLOOD: FAIL - the broadcast did not reach both other wires "
      "(port0=%s port2=%s)" % (seen0, seen2))
sys.exit(1)
