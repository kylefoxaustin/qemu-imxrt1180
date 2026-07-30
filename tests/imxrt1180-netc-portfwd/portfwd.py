#!/usr/bin/env python3
# NETC switch WIRE->WIRE routing check (multi-physical-port).
#
# Each wire port is a POINT-TO-POINT UDP link to us -- NOT a shared multicast
# segment.  That is deliberate: QEMU forces IP_MULTICAST_LOOP=1 on every mcast
# netdev (net/socket.c), so a switch flooding a frame onto a shared mcast wire
# would re-ingest its own flood on that same group and storm -- a real loop, but
# not the topology under test.  A point-to-point udp= link (QEMU binds localaddr,
# sends to us; we bind our end, send to QEMU) never loops QEMU's egress back into
# QEMU, so the switch's forwarding is observed cleanly.
#
#   port 1  <->  QEMU localaddr Q1 / our PY1
#   port 2  <->  QEMU localaddr Q2 / our PY2
#
# We:
#   1. inject a frame SRC=MAC_B into port 2 so the switch learns MAC_B is on port 2,
#   2. inject a probe DST=MAC_B SRC=MAC_A into port 1,
# and assert the probe is routed OUT PORT 2 (appears on PY2), byte-exact.  That is
# one physical wire's frame switched onto another physical wire.
import socket, struct, sys, time

PY1, Q1 = 44011, 44012        # port 1: we recv on PY1, QEMU recv on Q1
PY2, Q2 = 44021, 44022        # port 2: we recv on PY2, QEMU recv on Q2
HOST = "127.0.0.1"
MAC_A = bytes.fromhex("020000000a0a")      # station on port 1
MAC_B = bytes.fromhex("020000000b0b")      # station on port 2
ET    = 0x88C0                             # a private ethertype for this test
PROBE = b"WIRE2WIRE-PROBE-PAYLOAD"

def frame(dst, src, body):
    return dst + src + struct.pack(">H", ET) + body

def mksock(recv_port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, recv_port))
    s.setblocking(False)
    return s

p1, p2 = mksock(PY1), mksock(PY2)          # our ends of the two wires
learn = frame(MAC_A, MAC_B, b"LEARN-MAC_B-ON-PORT2")   # ingress port 2 -> learn MAC_B
probe = frame(MAC_B, MAC_A, PROBE)                     # ingress port 1 -> route to port 2

deadline = time.time() + 20
seen_on_p2 = False
while time.time() < deadline:
    p2.sendto(learn, (HOST, Q2))           # teach the switch MAC_B is on port 2
    time.sleep(0.05)
    p1.sendto(probe, (HOST, Q1))           # inject the probe on port 1
    time.sleep(0.15)
    try:
        while True:
            d, _ = p2.recvfrom(2048)       # drain port 2's wire
            if len(d) < 14:
                continue
            dst, src, et = d[0:6], d[6:12], struct.unpack(">H", d[12:14])[0]
            if et == ET and src == MAC_A and dst == MAC_B and d[14:] == PROBE:
                seen_on_p2 = True          # probe reached port 2's wire, unchanged
    except BlockingIOError:
        pass
    if seen_on_p2:
        break

if seen_on_p2:
    print("PORTFWD: PASS - a frame ingressing wire port 1 was routed out wire port 2 (byte-exact)")
    sys.exit(0)
print("PORTFWD: FAIL - the probe was not routed to port 2's wire")
sys.exit(1)
