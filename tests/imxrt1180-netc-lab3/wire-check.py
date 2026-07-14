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
import select
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
#     [24..27]  INCARNATION -- a per-boot nonce                    -> tells REBOOT from REPLAY
#     [28..63]  fill 0x5A, EVERY byte                              -> BAD_PATTERN
#     FRAME_LEN 64 exactly
#
# The incarnation is new (holobench's 4-node lab, 2026-07-14). A sequence number alone
# cannot survive a peer RESTART: mcx departs at t+420 and rejoins at t+480, its counter
# starts at 1, and our freshness check condemned it 8,982 times as a "stale buffer".
#   ⭐ A PEER THAT RESTARTED IS NOT A PEER THAT REPLAYED.
# LEGACY sentinel: a node without the field emits the old 0x5A fill from [24], so its
# "incarnation" reads 0x5A5A5A5A -- the same constant on every node and every boot. That
# is not a nonce, it is the ABSENCE of one, and such a peer must NOT be condemned.
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
FILL = 0x5A                              # at frame[28..63], every byte
LEGACY_INC = 0x5A5A5A5A                  # what a node WITHOUT the field emits at [24..27]
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


def beacon(src, et, seq, magic=MAGIC, self_et=None, inc=0xA11CE001):
    """A WELL-FORMED beacon -- magic, self-et, seq, incarnation, fill."""
    body = magic + struct.pack(">H", et if self_et is None else self_et) \
                 + struct.pack(">I", seq) + struct.pack(">I", inc)
    return frame(src, et, body, fill=FILL)


def legacy_beacon(src, et, seq):
    """A v1 body: magic, self-et, seq, then 0x5A ALL THE WAY from [24] -- no incarnation.
    Its [24..27] reads as the sentinel 0x5A5A5A5A."""
    body = MAGIC + struct.pack(">H", et) + struct.pack(">I", seq)
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
    bad = [i for i in range(28, FRAME_LEN) if f[i] != FILL]
    if bad:
        return ("BAD_PATTERN: [28..63] must be 0x5A fill; byte %d is 0x%02x"
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
    print("  ok  12 IPv6 (0x86DD) frames drew no CORRUPT")

    # ⚠ AND THAT ASSERTION ABOVE HAS ROTTED, SO DO NOT LEAN ON IT.
    #
    # It was a real test when the node CONDEMNED anything that was not its own ethertype.
    # But self-arming means an un-armed sender is NEVER condemned -- and IPv6 can never
    # arm -- so "no CORRUPT on IPv6" is now TRUE BY CONSTRUCTION and CANNOT FAIL. Deleting
    # the ethertype gate entirely does not make it fire.
    #
    #   ⭐ A NEGATIVE TEST ROTS GREEN WHEN THE MODEL IMPROVES UNDER IT. The behaviour that
    #      made it falsifiable was removed, and the assertion stayed, looking exactly as
    #      reassuring as the day it caught something.
    #
    # So assert the POSITIVE fact instead, which IS falsifiable: the IPv6 was SEEN and
    # DELIBERATELY IGNORED -- counted as foreign, never routed to a peer slot. Without the
    # gate, those frames land in the spare slot (and shut imx91 out of it).
    #
    #   ⭐ "A NEGATIVE RESULT IS ONLY A RESULT IF THE CONDITION WAS PRESENT. 'We never fired
    #      on IPv6' and 'there was no IPv6' are the same log." (91emulator)  The foreign
    #      COUNTER is what tells those two apart, and it is on the PASS line.
    ipv6_sent = 12

    # ═══ PHASE 2b ═══ imx91 (0x88B8) IS our protocol, and it must be READ.
    #
    # 91emulator, 2026-07-14: "Right now NOBODY on that segment checks my body -- not one
    # node -- and my beacon has never been read by an implementation I did not author."
    #
    # We were part of nobody: our gate was `et != PEER_A && et != PEER_B`, so 0x88B8 fell
    # out before we read byte 14.  Ignoring a peer and VALIDATING a peer are not the same
    # act, and only one of them is worth anything to the peer.
    #
    #   ⭐ VALIDATING A PEER IS NOT THE SAME AS DEPENDING ON ONE.  We do not require imx91
    #      for PASS (our contract is peers=2) -- but we read its body and we SAY SO.
    if not said(r"peer 0x88b8 body OK"):
        fail("the node never validated imx91's beacon (0x88B8).\n"
             "      12 WELL-FORMED 0x88B8 frames were put on the wire and the node said\n"
             "      nothing about any of them. A peer set that is two hardcoded constants\n"
             "      makes every future node a firmware release, and leaves that node with\n"
             "      no oracle but its own author.")
    print("  ok  imx91 (0x88B8) body READ and ACCEPTED: %s"
          % said(r"peer 0x88b8 body OK")[0].strip()[:78])

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

    m = re.search(r"foreign frames ignored: (\d+)", p[-1])
    if not m:
        fail("the PASS line does not report a foreign-frame count. Without it, 'we never "
             "fired on IPv6' and 'there was no IPv6' are the same log.")
    if int(m.group(1)) < ipv6_sent:
        fail("the node counted only %s foreign frames; we put %d IPv6 frames on the wire.\n"
             "      Either it never SAW them, or it saw them and did not recognise them as\n"
             "      not-our-protocol. Either way it cannot testify that the condition this\n"
             "      test depends on was ever present -- and 'we never fired on IPv6' and\n"
             "      'there was no IPv6' are then the same log." % (m.group(1), ipv6_sent))
    print("  ok  and it SAW the IPv6: %s foreign frames counted and ignored (>= %d sent)"
          % (m.group(1), ipv6_sent))

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

    # ═══ PHASE 5c ═══ A RESTART IS NOT A REPLAY. This is holobench's lab bug, reproduced.
    #
    # Peer A is armed (incarnation 0xA11CE001, seq up around 100+). Now it "reboots": a NEW
    # incarnation, and the sequence starts over at 1. The old code condemned this 8,982
    # times. The node must instead RESET its baseline and keep counting.
    time.sleep(0.8)          # let phase-5b's malformed frames drain first
    pump_console()
    before = len(said(r"ENET-LAB3 CORRUPT"))
    reboot_inc = 0xB007B007
    for i in range(10):
        send(beacon(MAC_A, ET_A, 1 + i, inc=reboot_inc))   # seq 1,2,3.. under a NEW boot
        time.sleep(0.05)
    time.sleep(1.0)
    pump_console()
    new_corrupt = said(r"ENET-LAB3 CORRUPT")[before:]
    replay_after_reboot = [c for c in new_corrupt if "0x88b5" in c and "REPLAY" in c]
    if replay_after_reboot:
        fail("A RESTARTED PEER WAS CONDEMNED AS A REPLAY.\n"
             "      Peer 0x88b5 rebooted (new incarnation, seq restarts at 1) and the node\n"
             "      called it a stale buffer:\n        %s\n\n"
             "      This is exactly the 8,982 false CORRUPTs holobench measured. A sequence\n"
             "      number alone is a claim about a PROCESS; the incarnation makes it a claim\n"
             "      about a PEER. A PEER THAT RESTARTED IS NOT A PEER THAT REPLAYED."
             % "\n        ".join(replay_after_reboot))
    if not said(r"peer 0x88b5 REBOOTED"):
        fail("the node did not recognise peer 0x88b5's restart. It should announce the new\n"
             "      incarnation and reset freshness, not silently accept OR condemn.")
    print("  ok  restart welcomed: %s" % said(r"peer 0x88b5 REBOOTED")[0].strip()[:76])

    # ═══ PHASE 5d ═══ ...but a STALE frame from the boot it LEFT is still a replay.
    #
    # The incarnation must not disarm the detector. A frame carrying the OLD incarnation,
    # after the peer has moved on, is a buffer from a boot that no longer exists -- a real
    # stale-buffer replay, and it must still be condemned.
    before = len(said(r"ENET-LAB3 CORRUPT"))
    for i in range(8):
        send(beacon(MAC_A, ET_A, 500 + i, inc=0xA11CE001))   # the incarnation it LEFT
        time.sleep(0.05)
    time.sleep(1.0)
    pump_console()
    stale = [c for c in said(r"ENET-LAB3 CORRUPT")[before:] if "REBOOTED OUT OF" in c]
    if not stale:
        fail("a frame carrying the incarnation the peer already REBOOTED OUT OF was NOT\n"
             "      condemned. The incarnation must distinguish reboot from replay in BOTH\n"
             "      directions -- otherwise it has simply switched the detector off.")
    print("  ok  stale frame from the abandoned boot -> still CORRUPT: %s"
          % stale[0].strip()[:70])

    # ═══ PHASE 5e ═══ A LEGACY PEER MUST NOT MASK A NON-CUTOVER.
    #
    # 95emulator corrected my "no flag day" claim: this IS a flag day. And my first cut had
    # the masking bug they named -- it counted a legacy peer and reported it VERIFIED, so the
    # node would pass GREEN over a peer that had not cut over.
    #
    #   ⭐ A CUTOVER THAT CANNOT GO RED IS ONE NOBODY CAN VERIFY. The failure to fear is a
    #      green that means a node quietly stayed on the old body.
    #
    # So: a fresh node, one GOOD peer (0x88b7) and one LEGACY peer (0x88b5, no incarnation).
    # The node must NOT PASS -- it cannot see both cut-over peers -- and it must say why.
    print("\n  -- phase 5e: a legacy (no-incarnation) peer must not be counted --")
    # NOTE: a SEPARATE node on its own group. We must NOT kill the main `qemu` here --
    # phases 6 and 7 still need it. (I did kill it, copying phase 8's pattern, and phases
    # 6/7 then ran against a dead node. An ordering bug is a test bug, and a test bug
    # manufactures a finding.)
    G3 = "230.0.0.%d" % random.randint(20, 219)
    P3 = random.randint(20000, 39000)
    rx3 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    rx3.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx3.bind(("", P3))
    rx3.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                   struct.pack("4sl", socket.inet_aton(G3), socket.INADDR_ANY))
    rx3.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    rx3.settimeout(0.05)
    q3 = subprocess.Popen(
        [QEMU, "-M", "mimxrt1180-evk", "-audio", "none", "-display", "none",
         "-monitor", "none", "-semihosting-config", "enable=on,target=native",
         "-kernel", ELF, "-nic", "socket,mcast=%s:%d,mac=54:27:8d:00:00:00" % (G3, P3),
         "-serial", "stdio"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    con3 = []

    def pump3():
        while select.select([q3.stdout], [], [], 0)[0]:
            ln = q3.stdout.readline()
            if not ln:
                break
            con3.append(ln.rstrip())

    try:
        # wait for the node's banner: a fresh QEMU needs its PHY link up before it drains RX
        t0 = time.time()
        while time.time() - t0 < 8 and not [l for l in con3 if "ENET-LAB3 UP:" in l]:
            pump3(); time.sleep(0.1)
        seq3 = 0
        t0 = time.time()
        while time.time() - t0 < 10:
            seq3 += 1
            rx3.sendto(beacon(MAC_B, ET_B, seq3), (G3, P3))            # GOOD, cut over
            rx3.sendto(legacy_beacon(MAC_A, ET_A, seq3), (G3, P3))     # LEGACY, no incarnation
            time.sleep(0.05)
            pump3()
        time.sleep(1.0)
        pump3()
    finally:
        q3.terminate()
        try:
            q3.wait(timeout=5)
        except subprocess.TimeoutExpired:
            q3.kill()

    passes3 = [l for l in con3 if "ENET-LAB3 PASS" in l]
    if passes3:
        fail("THE NODE PASSED WITH A LEGACY PEER PRESENT.\n"
             "      Peer 0x88b5 sent NO incarnation (it had not cut over), and the node "
             "passed anyway:\n        %s\n\n"
             "      That is the masking bug: a green that hides a node still on the old body.\n"
             "      A ratified cutover must be able to go RED." % passes3[-1].strip())
    if not [l for l in con3 if "LEGACY body" in l and "0x88b5" in l]:
        fail("the node did not announce that 0x88b5 is on the legacy body. It must say why\n"
             "      the segment is red, or a legacy peer is indistinguishable from a crash.")
    print("  ok  legacy peer NOT counted, no PASS, and the node said why: %s"
          % [l for l in con3 if "LEGACY body" in l][0].strip()[:72])

    # ═══ PHASE 6 ═══ a REPLAY (stale buffer) must be caught.
    before = len(said(r"ENET-LAB3 CORRUPT"))
    for i in range(8):
        send(beacon(MAC_B, ET_B, 50, inc=0xA11CE001))     # SAME incarnation, seq BACKWARDS
        time.sleep(0.05)
    time.sleep(1.0)
    pump_console()
    hits = [h for h in said(r"ENET-LAB3 CORRUPT")[before:] if "REPLAY" in h]
    if not hits:
        fail("a peer REPLAYED an old sequence number and the node did not notice.\n"
             "      A stale buffer holds a PREVIOUSLY VALID frame -- a checksum cannot\n"
             "      see it. Only a number going BACKWARDS can.")
    print("  ok  replayed seq -> CORRUPT fires: %s" % hits[0].strip())

    # ═══ PHASE 7 ═══ THE IMPOSTOR: 1000 bytes with a VALID 64-byte prefix.
    #
    # 91emulator, to me by name: "if you fixed the length but your permissiveness lives in a
    # LATCH or a fallback, your fix is a NO-OP and your suite will still be green. Send
    # yourselves a 1000-byte frame with a valid 64-byte prefix and check you go to ZERO."
    #
    # We did not go to zero. Before this fix: 2 PASS beats, 0 CORRUPT, and the node printed
    # "0x88b5 VERIFIED". Our check was `length >= 64`, not `== 64` -- while the README table
    # I had transcribed from mcx's source that same morning said "FRAME_LEN 64 exactly".
    #
    #   ⭐ FRAME_LEN IS A TERM OF THE CONTRACT, NOT A FLOOR.
    #   ⭐ AND THE LENGTH CHECK ALONE IS NOT THE FIX: an over-long frame has no valid body, so
    #      a self-arming latch files the liar as "phase-1, hasn't upgraded yet" and keeps
    #      counting it. THE MAGIC is what tells a BROKEN BEACON from a STRANGER.
    before_pass = len(said(r"ENET-LAB3 PASS"))
    before_corrupt = len(said(r"ENET-LAB3 CORRUPT"))

    def impostor(src, et, seq):
        """A perfectly valid 64-byte beacon... followed by 936 bytes of junk."""
        return beacon(src, et, seq) + bytes([0xAA]) * 936

    # ⚠ ARM THE TEST, AND PROVE IT ARMED, BEFORE BELIEVING ONE WORD OF THE RESULT.
    #
    # 95emulator's first run of this exact test ACCUSED THEIR OWN MODEL: the impostor flag
    # never reached the guest, so the "impostor" emitted ordinary 64-byte frames, their judge
    # correctly counted them, and the harness reported the MODEL was broken. They were one
    # commit from "fixing" a receiver that was already right -- and the false result agreed
    # with what 91 had just predicted, which is the hardest kind to catch.
    #
    #   ⭐ A NEGATIVE TEST THAT DID NOT PRODUCE THE CONDITION IT NAMES DOES NOT MERELY MISS A
    #      BUG -- IT MANUFACTURES ONE. AND THE FIX YOU THEN APPLY IS DAMAGE.
    #
    # So we do not assert that we MEANT to send 1000 bytes. We read our own frames back off
    # the wire and check what actually crossed it.
    armed_len = 0
    seq += 200
    for i in range(40):
        f_imp = impostor(MAC_A, ET_A, seq + i)
        send(f_imp)
        send(impostor(MAC_B, ET_B, seq + i))
        # bounded drain: we are flooding the group, so an unbounded recv never times out
        for _ in range(8):
            try:
                d = rx.recv(2048)
            except socket.timeout:
                break
            if (len(d) > FRAME_LEN and struct.unpack(">H", d[12:14])[0] == ET_A
                    and check_beacon(d[:FRAME_LEN], ET_A) is None):
                armed_len = len(d)      # a valid 64-byte prefix, and longer than 64
        pump_console()
    time.sleep(1.5)
    pump_console()

    if armed_len != 1000:
        fail("THE IMPOSTOR NEVER ARMED -- no 1000-byte frame with a valid 64-byte prefix was\n"
             "      observed on the wire (saw len=%d). This test cannot say anything about the\n"
             "      node, and a result read from it now would MANUFACTURE a bug in a model that\n"
             "      may be entirely correct." % armed_len)
    print("  ok  impostor ARMED: a %d-byte frame with a VALID 64-byte prefix is on the wire"
          % armed_len)

    # NOW the result means something.
    new_pass = len(said(r"ENET-LAB3 PASS")) - before_pass
    wrong_len = [c for c in said(r"ENET-LAB3 CORRUPT")[before_corrupt:] if "WRONG-LENGTH" in c]
    if new_pass:
        fail("WE COUNTED THE LIAR: %d PASS beat(s) while BOTH peers were emitting 1000-byte\n"
             "      frames that every other node on the segment rejects.\n"
             "      A receiver more permissive than the segment counts peers everyone else is\n"
             "      throwing away -- and then OUR green is the lie, because ours is the only\n"
             "      one that came back." % new_pass)
    if not wrong_len:
        fail("the node neither counted the impostor NOR condemned it. A frame carrying\n"
             "      0xB5B6B7C0 IS speaking the protocol -- it is just speaking it WRONG, and\n"
             "      that is a BROKEN BEACON, not an un-upgraded peer. It must be CORRUPT.")
    print("  ok  impostor: 0 PASS beats, %d condemned -- %s"
          % (len(wrong_len), wrong_len[0].strip()[:70]))

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

    # ═══ PHASE 8 ═══ THE IMPOSTOR THAT NEVER SPOKE THE PROTOCOL PROPERLY. FRESH NODE.
    #
    # Phase 7 is NOT sufficient and I nearly shipped it as if it were. By then peers A and B
    # are ARMED (they sent good beacons in phase 4), so the length check alone condemns the
    # impostor and THE LATCH IS NEVER CONSULTED. But 91's bug is exactly the case where the
    # latch IS consulted: a peer whose ONLY output is 1000-byte frames has never emitted a
    # valid body, so `*armed` is 0, and a receiver that routes malformed-with-magic through
    # the latch files it as "phase-1, hasn't upgraded yet" AND KEEPS COUNTING IT.
    #
    #   ⭐ A TEST THAT CANNOT REACH THE STATE THE BUG LIVES IN IS NOT A TEST OF THAT BUG --
    #      however loudly it exercises the same line of code.
    #
    # So: a FRESH node, and the impostors speak nothing but 1000-byte frames from the first
    # packet they ever send. This is the only configuration in which the latch gets a vote.
    print("\n  -- phase 8: fresh node; impostors that NEVER emit a valid body --")
    qemu.terminate()
    try:
        qemu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu.kill()

    G2 = "230.0.0.%d" % random.randint(20, 219)
    P2 = random.randint(20000, 39000)
    rx2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    rx2.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx2.bind(("", P2))
    rx2.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                   struct.pack("4sl", socket.inet_aton(G2), socket.INADDR_ANY))
    rx2.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    rx2.settimeout(0.05)

    q2 = subprocess.Popen(
        [QEMU, "-M", "mimxrt1180-evk", "-audio", "none", "-display", "none",
         "-monitor", "none", "-semihosting-config", "enable=on,target=native",
         "-kernel", ELF,
         "-nic", "socket,mcast=%s:%d,mac=54:27:8d:00:00:00" % (G2, P2),
         "-serial", "stdio"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    con2 = []

    def pump2():
        while select.select([q2.stdout], [], [], 0)[0]:
            line = q2.stdout.readline()
            if not line:
                break
            con2.append(line.rstrip())

    armed2 = 0
    try:
        for i in range(80):
            rx2.sendto(impostor(MAC_A, ET_A, 1 + i), (G2, P2))
            rx2.sendto(impostor(MAC_B, ET_B, 1 + i), (G2, P2))
            for _ in range(8):
                try:
                    d = rx2.recv(2048)
                except socket.timeout:
                    break
                if (len(d) > FRAME_LEN and struct.unpack(">H", d[12:14])[0] == ET_A
                        and check_beacon(d[:FRAME_LEN], ET_A) is None):
                    armed2 = len(d)
            time.sleep(0.05)
            pump2()
        time.sleep(1.5)
        pump2()
    finally:
        q2.terminate()
        try:
            q2.wait(timeout=5)
        except subprocess.TimeoutExpired:
            q2.kill()

    if armed2 != 1000:
        fail("PHASE 8 NEVER ARMED: no 1000-byte impostor frame reached the wire (saw %d).\n"
             "      A result read from this run would manufacture a bug." % armed2)

    passes2 = [l for l in con2 if "ENET-LAB3 PASS" in l]
    corrupt2 = [l for l in con2 if "ENET-LAB3 CORRUPT" in l]
    if passes2:
        fail("THE LATCH EXCUSED THE LIAR.\n"
             "      Both peers emitted NOTHING BUT 1000-byte frames -- they never once spoke\n"
             "      the agreed body -- and the node PASSED %d time(s):\n        %s\n\n"
             "      This is 91emulator's finding exactly: an over-long frame has no valid\n"
             "      body, so a self-arming latch asks 'has this peer ever emitted one?',\n"
             "      sees NO, and files a peer spraying 1000 bytes of garbage as PHASE-1.\n"
             "      The length check is a NO-OP unless the MAGIC decides the classification:\n"
             "      a frame carrying 0xB5B6B7C0 is speaking the protocol, just speaking it\n"
             "      WRONG -- a BROKEN BEACON, not a stranger."
             % (len(passes2), passes2[0].strip()))
    if not [c for c in corrupt2 if "WRONG-LENGTH" in c]:
        fail("a never-armed peer sent 80 over-long beacons and the node neither counted nor\n"
             "      condemned them. Silence is not a verdict.")
    print("  ok  never-armed impostor: 0 PASS beats, %d condemned (the latch got a vote and "
          "correctly did NOT excuse it)" % len(corrupt2))

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
