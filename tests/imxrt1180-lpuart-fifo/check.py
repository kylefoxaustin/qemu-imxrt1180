#!/usr/bin/env python3
"""
THE LPUART RX FIFO -- AND THE WATERMARK NOTHING IN THIS TREE EVER SET.

PARAM and FIFO both advertise a 16-deep RX FIFO, and the SDK is compiled against
FSL_FEATURE_LPUART_FIFO_SIZEn(x) == 16.  This model implemented A ONE-BYTE HOLDING
REGISTER.

    ⭐ A CAPABILITY REGISTER IS A CONTRACT.  WE PROMISED SIXTEEN AND DELIVERED ONE.

And it was invisible, because the RM defines RDRF as

    "datawords in the receive buffer GREATER THAN WATER[RXWATER]"

and RXWATER RESETS TO ZERO.  With no watermark set, a 1-deep receiver and a 16-deep
one are BEHAVIOURALLY IDENTICAL -- and nothing in 24 test suites ever set one.

    ⭐ THE PROMISE WAS NEVER CALLED IN.  A CAPABILITY NOBODY EXERCISES AND A FRAME
       NOBODY INSPECTS ARE THE SAME BUG: the system is not correct, it is UNTESTED IN
       THE ONE DIMENSION IT CLAIMS.   (mcxn947qemu, who hit this on their own console.)

A driver that DOES call it in -- RXFE=1, RXWATER=3, wait for RDRF, read 4 -- would be
woken on the FIRST byte by the old model and would read THREE STALE BYTES.  Silent
corruption, on the most-exercised block in the tree.

So this test calls the promise in.  It drives the LPUART over a real chardev socket and
asserts the things a 1-deep receiver CANNOT do:

    1. RDRF stays CLEAR while count <= RXWATER, and asserts only when it is EXCEEDED
    2. WATER[RXCOUNT] reports the LIVE depth (it used to read 0 forever)
    3. all four bytes come back IN ORDER and BYTE-EXACT -- not one byte and three ghosts
    4. FIFO[RXFLUSH] actually empties it
    5. an empty receiver reports DATA[RXEMPT], not a phantom NUL

Verdicts:  0 PASS   1 WRONG   2 CANNOT TELL
"""
import os, socket, subprocess, sys, threading, time

QEMU    = os.environ.get("QEMU", "../../build/qemu-system-arm")
TIMEOUT = int(os.environ.get("TIMEOUT", "60"))
EXIT_PASS, EXIT_LIES, EXIT_CANNOT_TELL = 0, 1, 2

LPUART1  = 0x44380000
R_STAT, R_CTRL, R_DATA, R_FIFO, R_WATER = 0x14, 0x18, 0x1C, 0x28, 0x2C

STAT_RDRF   = 0x00200000
CTRL_RE     = 0x00040000
DATA_RXEMPT = 0x00001000
FIFO_RXFE   = 0x00000008
FIFO_RXFLUSH = 0x00004000
FIFO_RXEMPT = 0x00400000


def qtest(script, feed=None):
    """Drive qtest, and (optionally) push bytes into LPUART1's chardev first."""
    sock = "/tmp/rt1180-lpuart-fifo-%d.sock" % os.getpid()
    if os.path.exists(sock):
        os.unlink(sock)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock)
    srv.listen(1)

    p = subprocess.Popen(
        [QEMU, "-M", "mimxrt1180-evk", "-audio", "none", "-display", "none",
         "-accel", "qtest", "-qtest", "stdio", "-monitor", "none",
         "-serial", "unix:%s" % sock],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)

    conn, _ = srv.accept()
    answers = []

    # ONE READER AT A TIME.  The first version started the pump thread and THEN did a
    # synchronous readline() for the CTRL write's "OK" -- so two readers were racing on
    # the same stdout, the pump swallowed the CTRL ack, the answer count never lined up,
    # and the harness hung until the deadline.
    #
    #   A HARNESS DEADLOCK IS INDISTINGUISHABLE FROM A GUEST BUG, and it arrives
    #   disguised as your own success.  (91emulator: "I spent twenty minutes convinced
    #   a device model was aborting on a read.")  The model was fine.
    #
    # So: do the setup SYNCHRONOUSLY, feed the wire, and only THEN hand stdout to the
    # pump.  RE must be enabled before any byte arrives, because can_receive() gates on
    # it -- otherwise the chardev never delivers and the FIFO is empty for a reason that
    # has nothing to do with the FIFO.
    try:
        p.stdin.write("writel 0x%x 0x%x\n" % (LPUART1 + R_CTRL, CTRL_RE))
        p.stdin.flush()
        p.stdout.readline()                      # its OK -- synchronously, no pump yet
        if feed:
            conn.sendall(feed)
            time.sleep(0.3)                      # let the chardev deliver
    except (BrokenPipeError, OSError):
        pass

    def pump():
        for line in p.stdout:
            if line.startswith("OK") or line.startswith("FAIL"):
                answers.append(line.strip())
                if len(answers) == len(script):
                    return

    t = threading.Thread(target=pump, daemon=True)
    t.start()

    try:
        p.stdin.write("\n".join(script) + "\n")
        p.stdin.flush()
    except (BrokenPipeError, OSError):
        pass

    t.join(TIMEOUT)
    wedged = t.is_alive()
    p.kill()
    p.wait()
    conn.close()
    srv.close()
    os.unlink(sock)

    if wedged or len(answers) != len(script):
        print("CANNOT TELL: asked %d, got %d answers in %ds."
              % (len(script), len(answers), TIMEOUT))
        print("      A wedged model looks BUSY, not BROKEN.  NOT a pass.")
        sys.exit(EXIT_CANNOT_TELL)

    return [int(a.split()[-1], 16) for cmd, a in zip(script, answers)
            if cmd.startswith("readl")]


bad = []

# ---------------------------------------------------------------------------
# THE TEST THE OLD MODEL COULD NOT SURVIVE: RXFE=1, RXWATER=3, feed FOUR bytes.
#
# silicon : RDRF asserts only when count(4) > water(3)  -> wake at the 4th byte
# 1-deep  : RDRF asserts on the FIRST byte              -> driver reads 4, gets 3 ghosts
# ---------------------------------------------------------------------------
WATER = 3
script = []
script += ["writel 0x%x 0x%x" % (LPUART1 + R_FIFO,  FIFO_RXFE)]
script += ["writel 0x%x 0x%x" % (LPUART1 + R_WATER, WATER << 16)]
script += ["readl 0x%x" % (LPUART1 + R_STAT)]      # [0] after 4 bytes: RDRF must be SET
script += ["readl 0x%x" % (LPUART1 + R_WATER)]     # [1] RXCOUNT must read 4
script += ["readl 0x%x" % (LPUART1 + R_DATA)] * 4  # [2..5] the four bytes, in order
script += ["readl 0x%x" % (LPUART1 + R_STAT)]      # [6] drained -> RDRF clear
script += ["readl 0x%x" % (LPUART1 + R_DATA)]      # [7] empty -> RXEMPT, not a NUL
script += ["readl 0x%x" % (LPUART1 + R_FIFO)]      # [8] empty -> FIFO.RXEMPT

v = qtest(script, feed=b"\xDE\xAD\xBE\xEF")

stat4, water4 = v[0], v[1]
data = [x & 0xFF for x in v[2:6]]
stat0, data_empty, fifo_empty = v[6], v[7], v[8]
rxcount = (water4 >> 24) & 0x1F

print("RXFE=1, RXWATER=%d, four bytes on the wire (DE AD BE EF)\n" % WATER)
print("  STAT.RDRF with 4 > water   : %s   (want SET -- the driver is woken)"
      % ("SET" if stat4 & STAT_RDRF else "CLEAR"))
print("  WATER.RXCOUNT              : %d   (want 4 -- it used to read 0 forever)" % rxcount)
print("  the four bytes, in order   : %s" % " ".join("%02X" % b for b in data))
print("  STAT.RDRF once drained     : %s   (want CLEAR)"
      % ("SET" if stat0 & STAT_RDRF else "CLEAR"))
print("  DATA when empty            : 0x%04X  (want RXEMPT 0x1000, NOT a phantom 0x00)"
      % data_empty)
print("  FIFO.RXEMPT when empty     : %s" % ("SET" if fifo_empty & FIFO_RXEMPT else "CLEAR"))

if not (stat4 & STAT_RDRF):
    bad.append("RDRF never asserted even though count(4) > RXWATER(3)")
if rxcount != 4:
    bad.append("WATER.RXCOUNT reads %d, want 4 -- the guest cannot size its read" % rxcount)
if data != [0xDE, 0xAD, 0xBE, 0xEF]:
    bad.append("the FIFO returned %s, want DE AD BE EF -- ONE REAL BYTE AND THREE GHOSTS"
               % " ".join("%02X" % b for b in data))
if stat0 & STAT_RDRF:
    bad.append("RDRF still SET after the FIFO was drained")
if data_empty != DATA_RXEMPT:
    bad.append("an empty receiver returned 0x%04X, not RXEMPT -- a phantom NUL" % data_empty)
if not (fifo_empty & FIFO_RXEMPT):
    bad.append("FIFO.RXEMPT clear on an empty FIFO")

# ---------------------------------------------------------------------------
# AND THE WATERMARK MUST ACTUALLY HOLD THE DRIVER BACK: 3 bytes, water 3 -> NOT ready.
# This is the assertion a 1-deep receiver fails by construction.
# ---------------------------------------------------------------------------
script2 = [
    "writel 0x%x 0x%x" % (LPUART1 + R_FIFO,  FIFO_RXFE),
    "writel 0x%x 0x%x" % (LPUART1 + R_WATER, 3 << 16),
    "readl 0x%x" % (LPUART1 + R_STAT),     # 3 bytes, water 3 -> 3 > 3 is FALSE -> CLEAR
    "readl 0x%x" % (LPUART1 + R_WATER),
    "writel 0x%x 0x%x" % (LPUART1 + R_FIFO, FIFO_RXFE | FIFO_RXFLUSH),
    "readl 0x%x" % (LPUART1 + R_WATER),    # flushed -> RXCOUNT 0
]
w = qtest(script2, feed=b"\x11\x22\x33")
stat3, water3, water_flushed = w[0], w[1], w[2]

print("\nRXWATER=3, only THREE bytes on the wire (11 22 33)")
print("  STAT.RDRF with 3 == water  : %s   (want CLEAR -- 3 > 3 is FALSE)"
      % ("SET" if stat3 & STAT_RDRF else "CLEAR"))
print("  WATER.RXCOUNT              : %d   (want 3)" % ((water3 >> 24) & 0x1F))
print("  RXCOUNT after FIFO.RXFLUSH : %d   (want 0)" % ((water_flushed >> 24) & 0x1F))

if stat3 & STAT_RDRF:
    bad.append("RDRF asserted at count(3) == RXWATER(3) -- THE DRIVER IS WOKEN EARLY. "
               "This is the 1-deep receiver's signature: it wakes on the first byte, "
               "the driver reads its watermark's worth, AND THE REST ARE STALE.")
if ((water3 >> 24) & 0x1F) != 3:
    bad.append("RXCOUNT reads %d, want 3" % ((water3 >> 24) & 0x1F))
if ((water_flushed >> 24) & 0x1F) != 0:
    bad.append("FIFO.RXFLUSH did not empty the FIFO")

if bad:
    print("\nFAIL: %d assertion(s)." % len(bad))
    for b in bad:
        print("    - %s" % b)
    sys.exit(EXIT_LIES)

print("\nPASS: the promise is kept. 16 deep, watermark-gated, live RXCOUNT, working flush.")
sys.exit(EXIT_PASS)
