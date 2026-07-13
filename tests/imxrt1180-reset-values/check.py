#!/usr/bin/env python3
"""Differential: read EVERY register at reset, diff against the REFERENCE MANUAL.

PORTED from mcxn947qemu (2026-07-12).  Their words, and we proved them the same day:

    "A test that gets its addresses from the MODEL is not a test, it is a MIRROR,
     and mutation testing is blind to it BY CONSTRUCTION."

Our eDMA channel registers were at the WRONG ADDRESSES for the life of the model --
inherited from the MCXN947 it was adapted from -- and every eDMA test was green and
all sixteen mutations came back "caught", because the tests took their addresses FROM
THE MODEL.  This gate is the only one in the tree that cannot be satisfied that way.

WHY THIS EXISTS (mcxn's original note, which applies to us verbatim).  Peripherals reset with

    memset(s->regs, 0, sizeof(s->regs));

which feels like a safe, neutral default.  It is not neutral.  It is 59 CLAIMS THAT
EVERY RESET VALUE IS ZERO, AND THE GUEST BELIEVES THEM.  Two of those claims turned
out to hard-fault real firmware:

  * SCG SIRCCSR resets to 0100_0020h.  Bit 5 (SIRC_CLK_PERIPH_EN) is SET OUT OF
    RESET.  CLOCK_GetFro12MFreq() reads it and returns 0 Hz when it is clear, so
    EVERY FlexComm driver init tripped assert(sourceClock_Hz > 0U) and HARD-FAULTED.
    Nothing in the guest's clock_config.c sets that bit BECAUSE ON SILICON IT IS
    ALREADY SET.
  * SCG's read path returned "always valid" for every oscillator, so the SDK
    reported an external crystal frequency FOR AN OSCILLATOR NOBODY TURNED ON.

    ⭐ A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM.  IT IS A CLAIM.

THE GOLDEN IS THE REFERENCE MANUAL (rm-golden.json, extracted from the MCX N RM
rev 7 register tables and checked in), read back at the addresses CMSIS gives.  So
this gate CANNOT BE SATISFIED BY THE MODEL AGREEING WITH ITSELF -- which is exactly
how the LPI2C sub-block sat at the wrong address for months with a green test: the
test had taken its addresses FROM THE MODEL.  A test that gets its addresses from
the model is not a test, it is a MIRROR, and mutation testing cannot see it because
mutating the model moves the mirror too.  This is the oracle nobody here authored.

ABOUT THE ALLOWLIST, which is the dangerous part of this file.

    "An independent whitelist doesn't merely MISS the bug -- IT CERTIFIES IT."
                                                       -- ollama_95_neutron

So the allowlist is built to SHRINK, and it fights back:

  * a mismatch NOT in the allowlist    -> FAIL (a new lie)
  * an allowlisted entry that now MATCHES -> FAIL ("this is fixed; delete the line")
  * the remaining count is PRINTED LOUDLY, every run.  No silent caps: a gate that
    quietly tolerates 400 known-wrong registers reads as "covered" when it is not.

COVERAGE IS PARTIAL AND SAID SO OUT LOUD: only registers whose RM table row parsed
cleanly AND that CMSIS attributes to exactly one peripheral are probed.  It is a
FLOOR on the bugs, not a ceiling.
"""
import json, os, subprocess, sys, threading

TIMEOUT = int(os.environ.get("TIMEOUT", "120"))   # hard kill; a wedged QEMU must not park us

# THREE VERDICTS, NOT TWO.  "It failed" and "I could not run" are different facts and
# must not share an exit code -- that is the whole of ollama_95_neutron's retraction.
EXIT_PASS        = 0
EXIT_LIES        = 1     # the model disagrees with the manual: a real finding
EXIT_CANNOT_TELL = 2     # the gate did not complete: a hang/crash, NOT a verdict

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.environ.get("QEMU", os.path.join(HERE, "..", "..", "build", "qemu-system-arm"))

if not os.access(QEMU, os.X_OK):
    print("SKIP: qemu not built at %s" % QEMU)
    sys.exit(0)

golden = json.load(open(os.path.join(HERE, "rm-golden.json")))

allow = {}
with open(os.path.join(HERE, "known-deviations.txt")) as f:
    for line in f:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        inst, reg, reason = (line.split(None, 2) + [""])[:3]
        allow[(inst, reg)] = reason


def probe(regs):
    """Ask, read exactly as many answers as questions, then kill it.

    There is no `quit` in the qtest protocol -- it answers "FAIL Unknown command" --
    so QEMU never exits and THE HARNESS must end it.  (For a long time a CRASHING
    QEMU was what ended these tests: without -accel qtest the vCPU free-runs from a
    zeroed vector table and dies of Lockup.  A crash and a pass must never be
    indistinguishable, and a free-running CPU is a SECOND WRITER to the address
    space under test.)
    """
    p = subprocess.Popen(
        [QEMU, "-M", "mimxrt1180-evk", "-display", "none", "-accel", "qtest",
         "-qtest", "stdio", "-monitor", "none", "-serial", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)

    # A HARD DEADLINE, BECAUSE THE SUBJECT CAN WEDGE THE GATE.
    #
    # ollama_95_neutron, 2026-07-12: "fail-safe assumes the gate RETURNS. NO VERDICT
    # AND STILL WORKING ARE THE SAME OBSERVATION." Their conformance probe called the
    # NPU; a hang-inducing shape did not reach the gate and get REJECTED -- IT WEDGED
    # THE GATE, which then never got to say no.
    #
    # This gate drives QEMU. A model bug that hangs the machine would park this loop
    # in readline() forever, and the harness would look BUSY, not BROKEN -- indefinite
    # silence read as "still working". So: a deadline, and a timeout is a DISTINCT
    # VERDICT, never silence.
    deadline = threading.Timer(TIMEOUT, p.kill)
    deadline.start()
    vals = []
    try:
        p.stdin.write("".join("readl 0x%x\n" % r["addr"] for r in regs))
        p.stdin.flush()
        while len(vals) < len(regs):
            line = p.stdout.readline()
            if not line:
                break                    # killed, or died: the caller counts answers
            if line.startswith("OK 0x"):
                vals.append(int(line.split()[1], 16))
    except (BrokenPipeError, OSError):
        # THE KILL LANDED MID-CONVERSATION. Swallow it and RETURN WHAT WE HAVE, so
        # the caller can say "I could not tell". The first version of this guard let
        # the BrokenPipeError escape as a TRACEBACK and exit(1) -- THE SAME EXIT CODE
        # AS A LEGITIMATE FAIL. Built the wedge guard, gave it the disease it exists
        # to prevent. (ollama_95_neutron: "an exit-1 CRASH and an exit-1 REFUSAL are
        # INDISTINGUISHABLE BY EXIT CODE ALONE. Assert the code you MEANT.")
        pass
    finally:
        deadline.cancel()
        p.kill()
        p.wait()
    return vals


vals = probe(golden)
if len(vals) != len(golden):
    print("CANNOT TELL: asked %d questions, got %d answers." % (len(golden), len(vals)))
    print("      QEMU hung, died, or was killed at the %ds deadline. This is NOT a" % TIMEOUT)
    print("      pass and it is NOT a reset-value failure -- it is the gate reporting")
    print("      that IT COULD NOT RUN. A truncated conversation and a correct one")
    print("      differ only in the answers you never notice are missing.")
    sys.exit(EXIT_CANNOT_TELL)

mismatched = {(r["inst"], r["reg"]): (r, v)
              for r, v in zip(golden, vals) if v != r["reset"]}

new  = [k for k in mismatched if k not in allow]

# An allowlisted register that now agrees with the RM has been FIXED.  Say so, and
# FAIL, so the line gets deleted.  This is what stops the list from becoming a
# permanent certificate for 400 wrong answers.
#
# BUT: "it agrees with the RM" and "the gate CANNOT SEE IT ANY MORE" are different
# facts, and the first version of this line conflated them.  `k not in mismatched`
# is true both for a register that got FIXED and for one that FELL OUT OF THE GOLDEN
# -- so a coverage regression announced itself as three free victories.  It did
# exactly that to me tonight, and I only noticed because I diffed the golden.
#
#   AN ENTRY THAT VANISHED FROM THE ORACLE IS INDISTINGUISHABLE FROM ONE THAT PASSED,
#   UNLESS YOU ASK WHETHER IT WAS EVEN LOOKED AT.
covered = {(r["inst"], r["reg"]) for r in golden}
stale   = [k for k in allow if k in covered and k not in mismatched]
uncov   = [k for k in allow if k not in covered]

print("probed %d registers against the RM (golden = the reference manual)" % len(golden))
print("  matching        : %d" % (len(golden) - len(mismatched)))
print("  known deviations: %d   <-- THIS NUMBER MUST GO DOWN" % (len(mismatched) - len(new)))

rc = 0
if new:
    print("\nFAIL: %d register(s) disagree with the RM and are NOT in the allowlist." % len(new))
    print("      The guest reads a value the silicon would never produce.")
    cap = len(new) if os.environ.get("DUMP_ALL") else 25
    for inst, reg in sorted(new)[:cap]:
        r, v = mismatched[(inst, reg)]
        print("        %-12s %-18s @0x%08x  model=0x%08x  RM=0x%08x"
              % (inst, reg, r["addr"], v, r["reset"]))
    if len(new) > cap:
        print("        ... and %d more   (DUMP_ALL=1 to see them all)" % (len(new) - cap))
    rc = EXIT_LIES

if stale:
    print("\nFAIL: %d allowlisted register(s) now MATCH the RM -- they are FIXED." % len(stale))
    print("      Delete them from known-deviations.txt.  An allowlist that never")
    print("      shrinks stops being a to-do list and becomes a CERTIFICATE.")
    scap = len(stale) if os.environ.get("DUMP_ALL") else 25
    for inst, reg in sorted(stale)[:scap]:
        print("        %-12s %s" % (inst, reg))
    if len(stale) > scap:
        print("        ... and %d more   (DUMP_ALL=1 to see them all)" % (len(stale) - scap))
    rc = EXIT_LIES

if uncov:
    print("\nFAIL: %d allowlisted register(s) are NO LONGER COVERED by the golden." % len(uncov))
    print("      These did not get FIXED -- THE GATE STOPPED LOOKING AT THEM. That is a")
    print("      coverage REGRESSION, and it is the one failure that would otherwise")
    print("      read as good news.")
    for inst, reg in sorted(uncov)[:25]:
        print("        %-12s %s" % (inst, reg))
    rc = EXIT_LIES

if rc == 0:
    print("\nPASS: no new reset-value lies; %d known deviations, all still known."
          % len(mismatched))
sys.exit(rc)
