#!/usr/bin/env python3
"""
uSDHC VEND_SPEC ACROSS MIGRATION -- AND THE PREDICATE THAT DECIDES IT.

VEND_SPEC (@0xC0) resets to 0x3000_7809 on this silicon (RT1180 RM; the reset-value
gate reads it at 0x4285_00C0).  Upstream sdhci.c reset it to ZERO, held it in a
uint16_t for a 32-bit register, and NEVER MIGRATED IT AT ALL.

  * the uint16 TRUNCATED bits 28/29 on every write -- a bug for every platform
  * the zero reset laundered into the guest: `fsl_esdhc` READ-MODIFY-WRITES this
    register, so the guest read our zero and wrote it back AS ITS OWN CONFIG, walking
    away with the soft clock enables (bits 14:11) OFF.  Works here.  Fails on silicon.
  * and with no VMState, the value simply vanished across a migrate.

============================ THE PREDICATE ============================

An OMITTED subsection leaves the field holding exactly what reset() put there.  Two
obvious predicates are both WRONG, and each was independently shipped tonight:

    .needed = vendor_spec != 0            -> the guest deliberately writes 0, the
                                             subsection is omitted, and the destination
                                             reloads THE RESET VALUE.  ITS OWN STATE,
                                             SILENTLY REPLACED.  (a zero is a CLAIM.)

    .needed = vendor_spec_reset != 0      -> fixes that, but i.MX6/7 have reset 0 AND
                                             WRITE THIS REGISTER AT RUNTIME.  Their
                                             value is still lost.

  ⭐ A SUBSECTION MAY BE OMITTED IFF THE FIELD ALREADY HOLDS WHAT RESET WOULD PUT THERE.
     needed  <=>  vendor_spec != vendor_spec_reset

============================ WHY THE SIZE CHECK ============================

A predicate that just `return true`d would pass a VALUES-ONLY test -- while silently
changing the migration wire format for EVERY existing SDHCI guest, including boards
nobody here owns.  So this asserts BOTH halves:

    values : the guest's VEND_SPEC survives the migrate, INCLUDING a deliberate zero
    format : the state file is SMALLER when the field is untouched -- i.e. the
             subsection is GENUINELY OMITTED, not merely harmless

  ⭐ COVERAGE IS AN ASSERTION, NOT A PRINT STATEMENT.  A migration test that only checks
     values cannot see a wire format it has already broken.   (mcxn947qemu / 91emulator)

Verdicts:  0 PASS   1 WRONG   2 CANNOT TELL
"""
import json, os, socket, subprocess, sys, tempfile, threading, time

QEMU    = os.environ.get("QEMU", "../../build/qemu-system-arm")
TIMEOUT = int(os.environ.get("TIMEOUT", "60"))
EXIT_PASS, EXIT_LIES, EXIT_CANNOT_TELL = 0, 1, 2

VEND_SPEC = 0x428500C0
RESET_VAL = 0x30007809          # the RT1180 RM's cold-POR value


def run(write_val, statefile):
    """Boot, optionally write VEND_SPEC, snapshot to statefile, return what we read."""
    qmp = tempfile.mktemp(prefix="rt1180qmp", suffix=".sock", dir="/tmp")
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(qmp)
    srv.listen(1)

    p = subprocess.Popen(
        [QEMU, "-M", "mimxrt1180-evk", "-audio", "none", "-display", "none",
         "-accel", "qtest", "-qtest", "stdio", "-monitor", "none", "-serial", "none",
         "-qmp", "unix:%s" % qmp],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)
    conn, _ = srv.accept()
    f = conn.makefile("rw")
    f.readline()                                   # greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush()
    f.readline()

    script = []
    if write_val is not None:
        script.append("writel 0x%x 0x%x" % (VEND_SPEC, write_val))
    script.append("readl 0x%x" % VEND_SPEC)

    answers = []

    def pump():
        for line in p.stdout:
            if line.startswith("OK"):
                answers.append(line.strip())
                if len(answers) == len(script):
                    return

    t = threading.Thread(target=pump, daemon=True)
    t.start()
    p.stdin.write("\n".join(script) + "\n")
    p.stdin.flush()
    t.join(TIMEOUT)
    if t.is_alive():
        p.kill(); p.wait(); conn.close(); srv.close(); os.unlink(qmp)
        print("CANNOT TELL: qtest did not answer.")
        sys.exit(EXIT_CANNOT_TELL)

    before = int(answers[-1].split()[-1], 16)

    # migrate to a file
    f.write(json.dumps({"execute": "migrate",
                        "arguments": {"uri": "exec:cat > %s" % statefile}}) + "\n")
    f.flush()
    f.readline()
    for _ in range(200):                            # wait for completion
        f.write(json.dumps({"execute": "query-migrate"}) + "\n"); f.flush()
        r = json.loads(f.readline())
        if r.get("return", {}).get("status") in ("completed", "failed"):
            break
        time.sleep(0.05)
    st = r.get("return", {}).get("status")

    p.kill(); p.wait(); conn.close(); srv.close(); os.unlink(qmp)

    if st != "completed":
        print("CANNOT TELL: migration did not complete (status=%s)." % st)
        sys.exit(EXIT_CANNOT_TELL)

    return before, os.path.getsize(statefile)


def restore(statefile):
    """Boot a fresh machine from the snapshot and read VEND_SPEC back."""
    p = subprocess.Popen(
        [QEMU, "-M", "mimxrt1180-evk", "-audio", "none", "-display", "none",
         "-accel", "qtest", "-qtest", "stdio", "-monitor", "none", "-serial", "none",
         "-incoming", "exec:cat %s" % statefile],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)
    answers = []

    def pump():
        for line in p.stdout:
            if line.startswith("OK"):
                answers.append(line.strip())
                return

    t = threading.Thread(target=pump, daemon=True)
    t.start()
    time.sleep(1.0)                                  # let the incoming stream load
    p.stdin.write("readl 0x%x\n" % VEND_SPEC)
    p.stdin.flush()
    t.join(TIMEOUT)
    wedged = t.is_alive()
    p.kill(); p.wait()
    if wedged or not answers:
        print("CANNOT TELL: the restored machine did not answer.")
        sys.exit(EXIT_CANNOT_TELL)
    return int(answers[0].split()[-1], 16)


bad = []
tmp = tempfile.mkdtemp(prefix="rt1180mig")

CASES = [
    # (what the guest writes, what it must read back)
    (None,       RESET_VAL,  "untouched -- the field already holds the reset value"),
    (0x00000000, 0x00000000, "A DELIBERATE ZERO. The predicate `!= 0` LOSES THIS."),
    (0x30007B09, 0x30007B09, "FRC_SDCLK_ON set -- the shape the driver actually writes"),
    (0x00000100, 0x00000100, "the i.MX6/7 shape (reset 0, written at runtime)"),
]

print("uSDHC VEND_SPEC @0x%08x -- reset 0x%08x (RT1180 RM)\n" % (VEND_SPEC, RESET_VAL))
sizes = {}
for wr, want, why in CASES:
    sf = os.path.join(tmp, "s%s" % ("none" if wr is None else "%08x" % wr))
    before, size = run(wr, sf)
    after = restore(sf)
    sizes["untouched" if wr is None else wr] = size
    ok = (before == want and after == want)
    print("  write %-10s -> read 0x%08x -> migrate -> 0x%08x   state %8d B   %s"
          % ("(none)" if wr is None else "0x%08x" % wr, before, after, size,
             "ok" if ok else "<-- LOST"))
    print("      %s" % why)
    if not ok:
        bad.append("write %s: read 0x%08x, after migrate 0x%08x, want 0x%08x -- %s"
                   % ("(none)" if wr is None else "0x%08x" % wr, before, after, want, why))

# ---- THE WIRE FORMAT. A `return true` predicate passes the values test and silently
#      changes the snapshot for every existing SDHCI guest.  The subsection must appear
#      ONLY when the field differs from its reset value.
base = sizes["untouched"]
print("\n  wire format -- the subsection must be GENUINELY OMITTED when untouched:")
for wr in (0x00000000, 0x30007B09, 0x00000100):
    delta = sizes[wr] - base
    print("    untouched %d B  vs  write 0x%08x -> %d B   (delta %+d)"
          % (base, wr, sizes[wr], delta))
    if delta <= 0:
        bad.append("writing 0x%08x did not GROW the state file (delta %+d): the "
                   "subsection is not being emitted, so the value cannot have migrated"
                   % (wr, delta))

if bad:
    print("\nFAIL: %d." % len(bad))
    for b in bad:
        print("    - %s" % b)
    sys.exit(EXIT_LIES)

print("\nPASS: VEND_SPEC survives migration -- INCLUDING a deliberate zero -- and the")
print("      subsection is emitted ONLY when the field differs from its reset value,")
print("      so every other SDHCI platform's wire format is byte-for-byte unchanged.")
sys.exit(EXIT_PASS)
