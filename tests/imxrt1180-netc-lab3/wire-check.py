#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# THE LAB-3 NODE, ASSERTED FROM THE WIRE BY A PEER THAT IS NOT A COPY OF IT.
#
# ─────────────────────────────────────────────────────────────────────────────
# WHY THIS EXISTS, AND WHY THE OLD SELF-TEST COULD NOT HAVE FOUND THE BUG
#
# tools/netc-eth-lab3.sh's self-check builds THREE STAND-INS FROM THE SAME PATCH:
# one per EtherType, all three compiled from our own beacon code.  It was green for
# weeks while our node emitted a magic -- 'L','B','3','!' -- THAT THE FLEET NEVER
# AGREED TO.  Of course it was green: all three actors spoke it.
#
#   ⭐ A REHEARSAL WHOSE OTHER ACTORS ARE COPIES OF YOU CANNOT DISCOVER THAT YOU
#      DISAGREE WITH ANYONE.  It can only discover that you disagree with YOURSELF.
#
# holobench found it on a real 4-node segment (rt1180: 0 heartbeats, 327,704 frames
# rejected by mcx).  This test is the missing half: the peer is written in a DIFFERENT
# LANGUAGE, from the SPEC, and it does not import one line of the firmware's beliefs.
# It encodes the agreed constant as a literal, exactly as mcx and imx91 did -- the two
# implementations that interoperated first try, never having spoken.
#
#   ⭐ AND THE ASSERTION IS READ FROM THE SUBJECT, NOT THE OBSERVER: we do not grep the
#      ELF for 0xB5B6B7C0 (the compiler emits it as four byte-immediates -- an ELF grep
#      cannot see it, and would report a correct node as broken).  WE READ THE BYTES OFF
#      THE WIRE.  A finding read from the subject survives a bug in the observer.
#
# QEMU's `-nic socket,mcast=` carries ONE RAW ETHERNET FRAME PER UDP DATAGRAM, so a
# plain multicast socket is a full peer on the segment.
#
# Exit 0 = the node is interoperable.  Exit 1 = it is not.
import os
import random
import re
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.environ.get("QEMU", os.path.join(HERE, "../../build/qemu-system-arm"))
ELF = os.environ.get("ELF", os.path.join(HERE, "netc-lab3-0x88B6.elf"))

# ── THE CONTRACT.  Written from the fleet agreement, not from our source. ──────
MAGIC = bytes([0xB5, 0xB6, 0xB7, 0xC0])  # at frame[14..17], big-endian
ET_ME = 0x88B6  # rt1180  (the node under test)
ET_A = 0x88B5  # mcxn947 (peer A)
ET_B = 0x88B7  # imx95   (peer B)
ET_91 = 0x88B8  # imx91   -- a fleet node, but NOT one of our two required peers
ET_IP6 = 0x86DD  # IPv6    -- NOT our protocol.  Must never be judged.

MY_MAC = bytes([0x54, 0x27, 0x8D, 0x00, 0x00, 0x00])
MAC_A = bytes([0x02, 0x4D, 0x43, 0x58, 0x00, 0x01])
MAC_B = bytes([0x02, 0x49, 0x4D, 0x58, 0x95, 0x01])
MAC_91 = bytes([0x02, 0x49, 0x4D, 0x58, 0x91, 0x01])
BCAST = b"\xff" * 6

GROUP = "230.0.0.%d" % random.randint(20, 219)
PORT = random.randint(20000, 39000)


def frame(src, et, body=b"", pad_to=60):
    f = BCAST + src + struct.pack(">H", et) + body
    return f + b"\x00" * max(0, pad_to - len(f))


def beacon(src, et, seq, magic=MAGIC):
    return frame(src, et, magic + struct.pack(">I", seq))


def fail(msg):
    print("FAIL: %s" % msg)
    sys.exit(1)


# ── the peer socket: a real station on the segment ────────────────────────────
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
rx.bind(("", PORT))
rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
              struct.pack("4sl", socket.inet_aton(GROUP), socket.INADDR_ANY))
rx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
rx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
rx.settimeout(0.05)


def send(f):
    rx.sendto(f, (GROUP, PORT))


qemu = subprocess.Popen(
    [QEMU, "-M", "mimxrt1180-evk", "-audio", "none", "-display", "none",
     "-monitor", "none", "-semihosting-config", "enable=on,target=native",
     "-kernel", ELF,
     "-nic", "socket,mcast=%s:%d,mac=54:27:8d:00:00:00" % (GROUP, PORT),
     "-serial", "stdio"],
    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)

console = []


def pump_console():
    """Non-blocking-ish drain of whatever the node has said."""
    import select
    while select.select([qemu.stdout], [], [], 0)[0]:
        line = qemu.stdout.readline()
        if not line:
            break
        console.append(line.rstrip())


def said(pat):
    return [l for l in console if re.search(pat, l)]


try:
    # ═══ PHASE 1 ═══ the node's OWN frames.  Read the magic off the WIRE.
    #
    # This is the assertion holobench's matrix was making from the outside, and the one
    # our whole self-test was structurally unable to make: DOES THIS NODE EMIT THE BODY
    # THE FLEET AGREED ON?
    seen, seqs = [], []
    t0 = time.time()
    while time.time() - t0 < 12 and len(seen) < 6:
        try:
            d = rx.recv(2048)
        except socket.timeout:
            pump_console()
            continue
        if len(d) < 22:
            continue
        et = struct.unpack(">H", d[12:14])[0]
        if et != ET_ME:
            continue          # not the node under test
        if d[6:12] != MY_MAC:
            continue
        seen.append(d)
        seqs.append(struct.unpack(">I", d[18:22])[0])
    pump_console()

    if not seen:
        fail("the node never transmitted a 0x88B6 frame at all "
             "(is the beacon running? is the wire up?)")

    bad = [f for f in seen if f[14:18] != MAGIC]
    if bad:
        got = bad[0][14:18]
        fail("THE MAGIC ON THE WIRE IS NOT THE AGREED MAGIC.\n"
             "      frame[14..17] = %s (%r)\n"
             "      expected      = %s  (0xB5B6B7C0)\n\n"
             "      This is the bug holobench's interop matrix found: a magic the fleet\n"
             "      did not agree on is a peer the node cannot hear. mcx and imx91 both\n"
             "      emit 0xB5B6B7C0 and interoperate; a node that does not is alone on a\n"
             "      wire it appears to be sharing."
             % (got.hex(), bytes(got), MAGIC.hex()))

    if seqs != sorted(seqs) or len(set(seqs)) != len(seqs):
        fail("the node's own sequence numbers are not strictly increasing: %s" % seqs)
    print("  ok  wire: %d frames, magic=0xB5B6B7C0, seq strictly increases %s"
          % (len(seen), seqs[:4]))

    # ═══ PHASE 2 ═══ IPv6 on the segment MUST NOT be judged.
    #
    # A real mixed segment carries the Linux peers' kernel NDP/MLD.  We used to
    # body-check it and report ENET-LAB3 CORRUPT.  No beacon-only suite can see this --
    # only a segment with a real network stack on it.  So we put one on the wire.
    #
    #   ⭐ A CORRUPTION DETECTOR THAT CRIES FOUL AT TRAFFIC THAT WAS NEVER ITS PROTOCOL
    #      WILL BE TURNED OFF BY THE PEOPLE IT PROTECTS.  (holobench)
    before = len(said(r"ENET-LAB3 CORRUPT"))
    for i in range(12):
        send(frame(MAC_B, ET_IP6, bytes([0x60, 0, 0, 0]) + os.urandom(40)))
        send(frame(MAC_91, ET_91, MAGIC + struct.pack(">I", i + 1)))  # a fleet node, not our peer
        time.sleep(0.02)
    time.sleep(1.5)
    pump_console()
    after = len(said(r"ENET-LAB3 CORRUPT"))
    if after > before:
        fail("the node reported CORRUPT on traffic that is NOT its protocol:\n        %s"
             % "\n        ".join(said(r"ENET-LAB3 CORRUPT")[before:]))
    print("  ok  12 IPv6 (0x86DD) + 12 imx91 (0x88B8) frames judged by nobody")

    # ═══ PHASE 3 ═══ an UN-UPGRADED peer must be counted, not condemned.
    #
    # Peer A speaks a WRONG body and has never spoken the right one.  Self-arming says:
    # that is an un-upgraded peer, not a corrupt frame.  Count PRESENCE; condemn nothing.
    #   ⭐ A RED YOU CANNOT TRUST IS WORSE THAN NO RED.  (holobench)
    before = len(said(r"ENET-LAB3 CORRUPT"))
    for i in range(10):
        send(frame(MAC_A, ET_A, b"IMX9" + struct.pack(">I", i + 1)))   # 95's ASCII body
        time.sleep(0.02)
    time.sleep(1.0)
    pump_console()
    if len(said(r"ENET-LAB3 CORRUPT")) > before:
        fail("the node CONDEMNED a peer that had never spoken the agreed body.\n"
             "      Self-arming must degrade to PRESENCE for an un-upgraded peer.\n        %s"
             % "\n        ".join(said(r"ENET-LAB3 CORRUPT")[before:]))
    print("  ok  un-upgraded peer (wrong body, never armed) counted, not condemned")

    # ═══ PHASE 4 ═══ two GOOD peers ⇒ PASS, and both must read VERIFIED.
    seq = 100
    t0 = time.time()
    while time.time() - t0 < 10 and not said(r"ENET-LAB3 PASS"):
        seq += 1
        send(beacon(MAC_A, ET_A, seq))
        send(beacon(MAC_B, ET_B, seq))
        time.sleep(0.05)
        pump_console()

    p = said(r"ENET-LAB3 PASS")
    if not p:
        fail("the node never PASSed against two peers emitting the AGREED body.\n"
             "      console tail:\n        %s" % "\n        ".join(console[-8:]))
    if "presence-only" in p[-1]:
        fail("the node PASSed but reports presence-only for a peer whose body it "
             "COULD verify:\n      %s" % p[-1])
    if p[-1].count("VERIFIED") != 2:
        fail("PASS does not report BOTH peers as content-VERIFIED:\n      %s" % p[-1])
    print("  ok  PASS with both peers VERIFIED: %s" % p[-1].strip())

    # ═══ PHASE 5 ═══ NOW peer A is ARMED.  Garbage from it IS a real corruption.
    #
    # This is the assertion earning its right to fire: the peer has PROVEN it can speak
    # the body, so a body-less frame from it can only be our RX path lying.
    # It also proves the detector is not merely switched off -- CAN THE NEGATIVE FAIL?
    before = len(said(r"ENET-LAB3 CORRUPT"))
    for i in range(10):
        send(frame(MAC_A, ET_A, b"\xde\xad\xbe\xef" + struct.pack(">I", seq + i + 1)))
        time.sleep(0.05)
    time.sleep(1.0)
    pump_console()
    hits = said(r"ENET-LAB3 CORRUPT")[before:]
    if not hits:
        fail("an ARMED peer sent a body-less frame and the node said NOTHING.\n"
             "      The corruption detector is decoration: it cannot fire.")
    print("  ok  ARMED peer sending garbage -> CORRUPT fires: %s" % hits[0].strip())

    # ═══ PHASE 6 ═══ a REPLAY (stale buffer) must be caught.
    before = len(said(r"ENET-LAB3 CORRUPT"))
    for i in range(8):
        send(beacon(MAC_B, ET_B, 50))     # goes BACKWARDS: last was >= 100
        time.sleep(0.05)
    time.sleep(1.0)
    pump_console()
    hits = [h for h in said(r"ENET-LAB3 CORRUPT")[before:] if "REPLAY" in h]
    if not hits:
        fail("a peer REPLAYED an old sequence number and the node did not notice.\n"
             "      A stale buffer holds a PREVIOUSLY VALID frame -- a checksum cannot\n"
             "      see it. Only a number going BACKWARDS can.")
    print("  ok  replayed seq -> CORRUPT fires: %s" % hits[0].strip())

    # ── the banner is a CONTRACT holobench parses.  Assert the exact grammar. ──
    up = said(r"ENET-LAB3 UP:")
    if not up:
        fail("the node printed no ENET-LAB3 UP: banner -- holobench derives the fleet "
             "status board from it, and cannot derive a board from prose.")
    if not re.search(r"^ENET-LAB3 UP: ethertype=0x88B6 peers=\d+ "
                     r"body=(emit|none) enforce=(self-arming|unconditional|none)\b",
                     up[0]):
        fail("the banner does not match holobench's published grammar:\n"
             "      got:  %s\n"
             "      want: ENET-LAB3 UP: ethertype=.. peers=.. body=emit|none "
             "enforce=self-arming|unconditional|none" % up[0])
    print("  ok  banner matches the fleet grammar: %s" % up[0].strip())

    print("\nPASS: the node is interoperable -- it emits the AGREED body, judges only "
          "its own protocol,\n      condemns only peers that proved they could do "
          "better, and its detectors CAN fire.")
    sys.exit(0)

finally:
    qemu.terminate()
    try:
        qemu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu.kill()
