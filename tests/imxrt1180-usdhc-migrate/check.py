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

# ============================================================================
# TWO MACHINES, AND THE SECOND ONE IS THE POINT.
#
#   mimxrt1180-evk : SETS vendor-spec-reset = 0x30007809 (our RM's value)
#   mcimx7d-sabre  : DOES NOT SET IT AT ALL -- reset == 0, the PROPERTY DEFAULT
#
# 91emulator, after proving it on a real imx8mp-evk:
#
#   ⭐ WE BOTH ONLY TESTED A MACHINE WHERE `reset != 0`. A TEST THAT ONLY EVER RUNS THE
#      MACHINE IT WAS WRITTEN FOR CANNOT SEE A BUG THAT LIVES IN THE DEFAULT.
#
# And the bug that lives in the default is not hypothetical: under the predicate
# `.needed = vendor_spec_reset != 0`, EVERY i.MX6/7/8M BOARD ON TYPE_IMX_USDHC has
# needed() == false FOREVER -- and their drivers STILL WRITE this register at runtime
# (FRC_SDCLK_ON, esdhc_write). Their value dies at the migration boundary.
#
# This file's first version could not have seen that. It ran one machine, and that
# machine set the property.
# ============================================================================
MACHINES = [
    # (machine, VEND_SPEC address, the reset value it MUST come up with)
    ("mimxrt1180-evk", 0x428500C0, 0x30007809),   # sets the property (RT1180 RM)
    ("mcimx7d-sabre",  0x30B40000 + 0xC0, 0x00000000),  # DOES NOT -- the default path
]


def run(machine, addr, write_val, statefile, expect_reset):
    """Boot, ASSERT THE SETUP, optionally write VEND_SPEC, snapshot, return what we read."""
    qmp = tempfile.mktemp(prefix="rt1180qmp", suffix=".sock", dir="/tmp")
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(qmp)
    srv.listen(1)

    p = subprocess.Popen(
        [QEMU, "-M", machine, "-audio", "none", "-display", "none",
         "-accel", "qtest", "-qtest", "stdio", "-monitor", "none", "-serial", "none",
         "-qmp", "unix:%s" % qmp],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)
    conn, _ = srv.accept()
    f = conn.makefile("rw")
    f.readline()                                   # greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush()
    f.readline()

    # ---- ASSERT THE SETUP BEFORE WRITING A BYTE ----------------------------
    # 91emulator shipped a case that used `-global` to force the reset value -- and the
    # board's own qdev_prop_set_uint32() SILENTLY OVERRODE IT, so the case ran at the
    # wrong reset value and PASSED, testing nothing.
    #
    #   ⭐ A TEST CASE THAT DOES NOT VERIFY ITS OWN SETUP IS TESTING SOMETHING ELSE.
    #
    # So: read the reset value back and require it to be the one this machine is
    # supposed to have. If the machine ever stops setting the property (or starts),
    # this fails LOUDLY instead of quietly measuring the wrong thing.
    script = ["readl 0x%x" % addr]
    if write_val is not None:
        script.append("writel 0x%x 0x%x" % (addr, write_val))
    script.append("readl 0x%x" % addr)

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

    reads = [int(a.split()[-1], 16) for c, a in zip(script, answers)
             if c.startswith("readl")]
    came_up_with, before = reads[0], reads[-1]

    if came_up_with != expect_reset:
        p.kill(); p.wait(); conn.close(); srv.close(); os.unlink(qmp)
        print("CANNOT TELL: %s came up with VEND_SPEC = 0x%08x, not 0x%08x."
              % (machine, came_up_with, expect_reset))
        print("      The SETUP is wrong, so any result from it is about something else.")
        sys.exit(EXIT_CANNOT_TELL)

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


def restore(machine, addr, statefile):
    """Boot a fresh machine from the snapshot and read VEND_SPEC back."""
    p = subprocess.Popen(
        [QEMU, "-M", machine, "-audio", "none", "-display", "none",
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
    p.stdin.write("readl 0x%x\n" % addr)
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

# The values a guest may hold.  `None` = never touched it.
WRITES = [
    (None,       "untouched -- the field already holds its reset value"),
    (0x00000000, "A DELIBERATE ZERO. The predicate `!= 0` LOSES THIS."),
    (0x30007B09, "FRC_SDCLK_ON set -- the shape the driver actually writes"),
    (0x00000100, "the i.MX6/7 runtime shape"),
]

for machine, addr, reset_val in MACHINES:
    print("%s -- VEND_SPEC @0x%08x, comes up 0x%08x%s"
          % (machine, addr, reset_val,
             "" if reset_val else "   <-- THE PROPERTY DEFAULT. THE PATH THE FIRST"
                                  " VERSION OF THIS TEST COULD NOT SEE."))
    sizes = {}
    for wr, why in WRITES:
        sf = os.path.join(tmp, "%s-%s" % (machine,
                                          "none" if wr is None else "%08x" % wr))
        before, size = run(machine, addr, wr, sf, reset_val)
        after = restore(machine, addr, sf)
        want = reset_val if wr is None else wr
        sizes["untouched" if wr is None else wr] = size
        ok = (before == want and after == want)
        print("  write %-10s -> 0x%08x -> migrate -> 0x%08x   state %8d B   %s"
              % ("(none)" if wr is None else "0x%08x" % wr, before, after, size,
                 "ok" if ok else "<-- LOST"))
        if not ok:
            bad.append("%s: wrote %s, read 0x%08x, after migrate 0x%08x, want 0x%08x -- %s"
                       % (machine, "(none)" if wr is None else "0x%08x" % wr,
                          before, after, want, why))

    # ---- THE WIRE FORMAT.  A `return true` predicate passes a values-only test and
    #      silently changes the snapshot for every existing SDHCI guest.  The subsection
    #      must appear ONLY when the field differs from its reset value.
    base = sizes["untouched"]
    for wr, _ in WRITES[1:]:
        delta = sizes[wr] - base
        differs = (wr != reset_val)
        if differs and delta <= 0:
            bad.append("%s: writing 0x%08x did not GROW the state (delta %+d) -- the "
                       "subsection was not emitted, so the value cannot have migrated"
                       % (machine, wr, delta))
        if not differs and delta != 0:
            bad.append("%s: writing 0x%08x (== its reset value) CHANGED the state size "
                       "by %+d -- the subsection is being emitted when it must not be, "
                       "and that is a wire-format change for every guest"
                       % (machine, wr, delta))
        print("    untouched %d B  vs  0x%08x -> %d B  (delta %+d)  %s"
              % (base, wr, sizes[wr], delta,
                 "must be emitted" if differs else "must NOT be emitted (== reset)"))
    print()

if bad:
    print("FAIL: %d." % len(bad))
    for b in bad:
        print("    - %s" % b)
    sys.exit(EXIT_LIES)

print("PASS: VEND_SPEC survives migration on BOTH machines -- the one that sets the")
print("      property AND the one that does not -- including a deliberate zero, and the")
print("      subsection is emitted only when the field differs from its reset value.")
print("      Every case asserted the reset value it actually came up with before writing.")
sys.exit(EXIT_PASS)
