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

# ── THE CONTRACT — TRANSCRIBED FROM A PEER'S SOURCE, NOT FROM A DESCRIPTION OF IT ──
#
#   mcxn947qemu/tests/mcxn-enet-lab3/main.c :: frame_ok()      (it interoperates; we did not)
#
#     [14..17]  magic 0xB5B6B7C0, big-endian                       -> BAD_MAGIC
#     [18..19]  SELF-ETHERTYPE — must equal [12..13], or the
#               frame CONTRADICTS ITSELF                           -> BAD_SELF_ET
#     [20..23]  monotonic sequence, big-endian                     -> BAD_REPLAY
#     [24..63]  fill 0x5A, EVERY byte                              -> BAD_PATTERN
#     FRAME_LEN 64 exactly
#
# ⚠ THE FIRST VERSION OF THIS FILE ENCODED ONLY THE MAGIC, AND PUT SEQ AT [18..21].
#   I took "magic = 0xB5B6B7C0 at [14..17]" out of a bus message, called it the spec, and
#   invented the other three fields to match our firmware.  The test passed.  It could not
#   have done anything else: I WROTE BOTH SIDES OF THE DISAGREEMENT.
#
#     ⭐ A PROSE SUMMARY OF A CONTRACT IS NOT THE CONTRACT.  The peers' SOURCE is — and it
#        was on this disk the whole time, one grep away.
#
#   Three of four fields were wrong.  mcx would have rejected every frame we sent EVEN WITH
#   THE MAGIC CORRECTED — our seq's high bytes landed in its SELF-ETHERTYPE field, and we
#   shipped 1000 bytes of SDK junk where [24..63] had to be 0x5A.  "Writing an independent
#   checker" is worth nothing if you derive its beliefs from the thing under test.
FRAME_LEN = 64
MAGIC = bytes([0xB5, 0xB6, 0xB7, 0xC0])  # at frame[14..17], big-endian
FILL = 0x5A                              # at frame[24..63], every byte
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


def frame(src, et, body=b"", fill=0x00):
    """A raw frame: [0..5] dst, [6..11] src, [12..13] et, then <body>, padded to 64."""
    f = BCAST + src + struct.pack(">H", et) + body
    return f + bytes([fill]) * max(0, FRAME_LEN - len(f))


def beacon(src, et, seq, magic=MAGIC, self_et=None):
    """A WELL-FORMED beacon, built to mcx's frame_ok() -- all four fields."""
    body = magic + struct.pack(">H", et if self_et is None else self_et) \
                 + struct.pack(">I", seq)
    return frame(src, et, body, fill=FILL)


def check_beacon(f, et):
    """mcx's frame_ok(), transcribed. Returns None if good, else the reason it rejects."""
    if len(f) != FRAME_LEN:
        return "FRAME_LEN is %d, the contract says %d" % (len(f), FRAME_LEN)
    if f[14:18] != MAGIC:
        return "BAD_MAGIC: [14..17] = %s, want %s" % (f[14:18].hex(), MAGIC.hex())
    self_et = struct.unpack(">H", f[18:20])[0]
    if self_et != et:
        return ("BAD_SELF_ET: [18..19] declares 0x%04x but [12..13] says 0x%04x "
                "-- the frame contradicts itself" % (self_et, et))
    bad = [i for i in range(24, FRAME_LEN) if f[i] != FILL]
    if bad:
        return ("BAD_PATTERN: [24..63] must be 0x5A fill; byte %d is 0x%02x"
                % (bad[0], f[bad[0]]))
    return None


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
        seqs.append(struct.unpack(">I", d[20:24])[0])
    pump_console()

    if not seen:
        fail("the node never transmitted a 0x88B6 frame at all "
             "(is the beacon running? is the wire up?)")

    # EVERY field, judged by mcx's own checker. Not "does it look like ours" --
    # "WOULD THE NODE THAT REJECTED US 327,704 TIMES ACCEPT THIS FRAME?"
    why = check_beacon(seen[0], ET_ME)
    if why:
        fail("mcx's frame_ok() WOULD REJECT THE FRAMES WE PUT ON THE WIRE.\n"
             "      %s\n\n"
             "      frame (%d B): %s\n\n"
             "      This is the assertion holobench's interop matrix was making from the\n"
             "      outside. A node that emits a body its peers throw away is alone on a\n"
             "      wire it appears to be sharing."
             % (why, len(seen[0]), seen[0].hex(" ")))

    if seqs != sorted(seqs) or len(set(seqs)) != len(seqs):
        fail("the node's own sequence numbers are not strictly increasing: %s" % seqs)
    print("  ok  wire: %d frames ACCEPTED by mcx's frame_ok() -- magic, self-et, "
          "0x5A fill, len=64" % len(seen))
    print("      seq at [20..23] strictly increases %s" % seqs[:4])

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
        send(beacon(MAC_91, ET_91, i + 1))   # a WELL-FORMED fleet node, but not one of our peers
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
        send(frame(MAC_A, ET_A, b"IMX9" + struct.pack(">I", i + 1)))   # 95's old ASCII body
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

    # ═══ PHASE 5b ═══ the RX side must enforce the WHOLE body, not just the magic.
    #
    # A receiver that checks only the magic will happily COUNT a frame that every other
    # node on the segment is THROWING AWAY -- and then report a peer sighting nobody else
    # agrees happened.  mcx rejects BAD_SELF_ET and BAD_PATTERN; so must we.
    # (Peer A is ARMED by now, so a malformed frame from it is a real corruption.)
    for label, f in [
        ("self-et contradicts the header",
         beacon(MAC_A, ET_A, seq + 50, self_et=0x9999)),
        ("0x5A fill is wrong",
         beacon(MAC_A, ET_A, seq + 51)[:30] + b"\x00" * 34),
    ]:
        before = len(said(r"ENET-LAB3 CORRUPT"))
        for _ in range(8):
            send(f)
            time.sleep(0.05)
        time.sleep(1.0)
        pump_console()
        if len(said(r"ENET-LAB3 CORRUPT")) == before:
            fail("an ARMED peer sent a frame where %s, and the node ACCEPTED it.\n"
                 "      mcx's frame_ok() rejects this frame. A receiver that checks only\n"
                 "      the magic counts peers that every other node is throwing away."
                 % label)
        print("  ok  ARMED peer, %s -> rejected" % label)

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
