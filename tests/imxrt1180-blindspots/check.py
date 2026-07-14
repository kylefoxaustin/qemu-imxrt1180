#!/usr/bin/env python3
"""
THE REGISTERS NO AUTOMATED GATE IN THIS TREE CAN WATCH.

The reset-value gate builds its golden from the RM's reset COLUMN.  For 87 rows the
manual DECLINES TO ANSWER -- it prints "See section" instead of a value, because the
reset depends on the instance, or is spelled out only in a bit diagram.  The extractor
REFUSES those rows, and that refusal is CORRECT: a wrong golden makes the checker lie.

    ⭐ BUT A REFUSAL IS NOT A CHECK.  (mcxn947qemu, 2026-07-13)
       "The registers it refused still needed one, and nobody had written it."

Their refusal pile was hiding their SWD debug pins.  MINE WAS HIDING USBPHY CTRL --
the register that holds the USB PHY in SOFT RESET AND CLOCK-GATED out of reset.  With
it invisible, this model came up as a board that had already booted.

    ⭐ BLOCKS ARE BORN HELD IN RESET.  A MODEL THAT SKIPS THAT IS NOT SIMPLER --
       IT IS A MODEL OF A BOARD THAT HAS ALREADY BOOTED.

So: HAND-READ from the RM, cross-checked against the CMSIS field masks, asserted here.
Every entry cites the section it came from.  This file is the only place in the tree
where a value is typed by hand, and that is exactly why each one carries its receipt.

Verdicts:  0 PASS   1 WRONG VALUE   2 CANNOT TELL (the gate could not run)
"""
import os, subprocess, sys, threading

QEMU    = os.environ.get("QEMU", "../../build/qemu-system-arm")
TIMEOUT = int(os.environ.get("TIMEOUT", "60"))
EXIT_PASS, EXIT_LIES, EXIT_CANNOT_TELL = 0, 1, 2

USBPHY1 = 0x42CA0000
USBPHY2 = 0x42CB0000

# (name, addr, expected, why-we-believe-it)
CASES = [
    ("USBPHY1 CTRL", USBPHY1 + 0x30, 0xC0000000,
     "RM 58.7.5 bit diagram: bit31 SFTRST reset 1, bit30 CLKGATE reset 1. "
     "PERI_USBPHY.h: SFTRST_MASK 0x80000000, CLKGATE_MASK 0x40000000. "
     "THE PHY IS HELD IN RESET AND GATED UNTIL FIRMWARE RELEASES IT."),
    ("USBPHY2 CTRL", USBPHY2 + 0x30, 0xC0000000,
     "same IP, second instance"),

    # The SET/CLR/TOG aliases READ THE BASE.  If they ever stop doing so, the guest's
    # release sequence (CTRL_CLR = SFTRST) silently does nothing -- 91emulator's i.MX 91
    # CCM bug, where "every write through the SET alias was dropped on the floor".
    ("USBPHY1 CTRL_SET reads base", USBPHY1 + 0x34, 0xC0000000, "an alias reads its BASE"),
    ("USBPHY1 CTRL_CLR reads base", USBPHY1 + 0x38, 0xC0000000, "an alias reads its BASE"),
    ("USBPHY1 CTRL_TOG reads base", USBPHY1 + 0x3C, 0xC0000000, "an alias reads its BASE"),

    # ── SAI PARAM: FOUR INSTANCES, FOUR DIFFERENT VALUES, AND THE GATE CANNOT SEE ANY
    #
    # The RM's SAI register table gives PARAM's reset as literally "See section" -- so
    # extract-rm-golden.py refuses it, CORRECTLY, and the reset-value gate has been
    # green about this register by NEVER LOOKING AT IT.
    #
    #   ⭐ A REFUSAL IS NOT A CHECK.
    #
    # The actual values are printed two pages later, per instance. Under that refusal,
    # the model advertised ONE hand-written constant to all four instances:
    #
    #       0x00050302   /* FIFO=32, channels=2 (best-effort) */
    #
    # PARAM[11:8] is log2 of the FIFO depth. 0x3 is EIGHT. The comment said 32. SAI1's
    # silicon has SIXTEEN. Three numbers and no two of them equal -- the same drifted
    # copy-paste mcxn947qemu found in imx93's SAI. A driver sizing a burst from PARAM
    # would have overrun a FIFO half the size it was promised.
    ("SAI1 PARAM", 0x443B0004, 0x00050402,
     "RM SAI chapter, 'Register reset values': SAI1 0005_0402h. FIFO=2^4=16 words, "
     "2 datalines. Confirmed by FSL_FEATURE_SAI_FIFO_COUNTn(SAI1)==16 and "
     "_CHANNEL_COUNTn(SAI1)==2, which the SDK driver is compiled against."),
    ("SAI2 PARAM", 0x42BB0004, 0x00050501,
     "RM: SAI2,SAI3 0005_0501h. FIFO=2^5=32, 1 dataline. THE INSTANCES ARE NOT THE "
     "SAME BLOCK, and PARAM is where they say so."),
    ("SAI3 PARAM", 0x42BC0004, 0x00050501, "RM: SAI2,SAI3 0005_0501h"),
    ("SAI4 PARAM", 0x42BD0004, 0x00050504,
     "RM: SAI4 0005_0504h. FIFO=2^5=32, FOUR datalines -- the model claimed 2."),

    ("SAI1 VERID", 0x443B0000, 0x03010000,
     "RM SAI register table: VERID reset 0301_0000h. (The model's old value was right, "
     "but it was labelled 'best-effort' -- so nobody knew it was.)"),
]

# And the RELEASE SEQUENCE the driver actually performs must actually work.
# USB_EhciPhyInit: CTRL_CLR = SFTRST; CTRL_CLR = CLKGATE.
SFTRST, CLKGATE = 0x80000000, 0x40000000


def qtest(script):
    p = subprocess.Popen(
        [QEMU, "-M", "mimxrt1180-evk", "-audio", "none", "-display", "none", "-accel", "qtest",
         "-qtest", "stdio", "-monitor", "none", "-serial", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)
    answers = []

    def pump():
        for line in p.stdout:
            if line.startswith("OK") or line.startswith("FAIL"):
                answers.append(line.strip())
                if len(answers) == len(script):
                    return

    t = threading.Thread(target=pump, daemon=True)
    t.start()
    # The writer runs in its own thread: a full pipe must block the WRITER, never the
    # READER, or a wedged subject deadlocks the harness and looks like a guest bug.
    def ask():
        try:
            p.stdin.write("\n".join(script) + "\n")
            p.stdin.flush()
            p.stdin.close()
        except (BrokenPipeError, OSError):
            pass
    threading.Thread(target=ask, daemon=True).start()

    t.join(TIMEOUT)
    wedged = t.is_alive()
    p.kill()
    p.wait()

    if wedged or len(answers) != len(script):
        print("CANNOT TELL: asked %d questions, got %d answers within %ds."
              % (len(script), len(answers), TIMEOUT))
        print("      A wedged model looks BUSY, not BROKEN. This is NOT a pass.")
        sys.exit(EXIT_CANNOT_TELL)

    return [a.split()[-1] for cmd, a in zip(script, answers)
            if cmd.startswith("readl")]


script = ["readl 0x%x" % addr for _, addr, _, _ in CASES]
# then drive the driver's own release sequence on USBPHY1 and re-read CTRL
script += [
    "writel 0x%x 0x%x" % (USBPHY1 + 0x38, SFTRST),    # CTRL_CLR = SFTRST
    "readl 0x%x" % (USBPHY1 + 0x30),
    "writel 0x%x 0x%x" % (USBPHY1 + 0x38, CLKGATE),   # CTRL_CLR = CLKGATE
    "readl 0x%x" % (USBPHY1 + 0x30),
]

vals = qtest(script)
reset_vals, after_sftrst, after_clkgate = vals[:len(CASES)], int(vals[-2], 16), int(vals[-1], 16)

print("%d hand-read registers the reset-value gate CANNOT SEE" % len(CASES))
print("  (the RM prints 'See section' in their reset column; the extractor refuses them,")
print("   correctly -- and A REFUSAL IS NOT A CHECK.)\n")

bad = []
for (name, addr, want, why), got in zip(CASES, reset_vals):
    got = int(got, 16)
    ok = got == want
    print("  %-28s @0x%08x  got 0x%08x  want 0x%08x  %s"
          % (name, addr, got, want, "ok" if ok else "<-- MISMATCH"))
    if not ok:
        bad.append((name, want, got, why))

print("\n  the driver's own release sequence (USB_EhciPhyInit):")
print("    CTRL_CLR = SFTRST   -> CTRL = 0x%08x  (want 0x%08x)" % (after_sftrst, CLKGATE))
print("    CTRL_CLR = CLKGATE  -> CTRL = 0x%08x  (want 0x%08x)" % (after_clkgate, 0))

if after_sftrst != CLKGATE or after_clkgate != 0:
    bad.append(("CTRL release sequence", 0, after_clkgate,
                "the guest clears SFTRST then CLKGATE through the CLR alias; if the "
                "alias does not modify the base, the PHY never leaves reset"))

if bad:
    print("\nFAIL: %d hand-read value(s) wrong." % len(bad))
    for name, want, got, why in bad:
        print("    %s: want 0x%08x, got 0x%08x" % (name, want, got))
        print("      %s" % why)
    sys.exit(EXIT_LIES)

print("\nPASS: the PHY is born held in reset and gated, and the driver's release works.")
sys.exit(EXIT_PASS)
