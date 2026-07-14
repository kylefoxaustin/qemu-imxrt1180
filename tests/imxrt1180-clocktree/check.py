#!/usr/bin/env python3
"""
CCM CLOCK-TREE VALUE GOLDEN.

Not "the frequency is non-zero".  Not "a bit got set".  THE EXACT HERTZ, derived
from first principles and from the SDK's own board config -- two sources, neither
of which is this model:

    SYS_PLL2  = XTAL * PLL_SYS2_528_MFI = 24 MHz * 22 = 528 MHz   (fsl_clock.h)
    Bus_Aon   = SysPll2Out / 4          = 132 MHz                 (clock_config.c:496)
    Gpt1      = SysPll3Div2 / 1         = 240 MHz                 (clock_config.c:580)
    SysPll3   = XTAL * PLL_SYS3_480_MFI = 24 MHz * 20 = 480 MHz; /2 = 240 MHz

A RANGE IS NOT A GOLDEN and one shape is not a golden either, so this sweeps the
axis the REGISTER exposes -- every MUX input and a spread of DIVs -- not merely the
one configuration the EVK firmware happens to use.  (A PWM golden at one prescaler
once passed a model with the modulo hardwired.)

THE BUG THIS EXISTS TO CATCH: the model used to store MUX/DIV and never read them.
Every timer ticked at a hardcoded constant -- GPT 10x slow, eFlexPWM 1.5x fast --
and the FOC value-golden could not see it, because that golden checks AMPLITUDE
(Ohm's law to one ADC count) and the error was in TIME.
"""
import json, os, subprocess, sys, threading

QEMU    = os.environ.get("QEMU", "../../build/qemu-system-arm")
TIMEOUT = int(os.environ.get("TIMEOUT", "60"))

CCM       = 0x44450000
ROOT_CTRL = lambda n: CCM + 0x0000 + n * 0x80          # CLOCK_ROOT[n].CONTROL
OBS_CTRL  = lambda n: CCM + 0x4400 + n * 0x80          # OBSERVE[n].CONTROL
OBS_FREQ  = lambda n: CCM + 0x4440 + n * 0x80          # OBSERVE[n].FREQUENCY_CURRENT

EXIT_PASS, EXIT_LIES, EXIT_CANNOT_TELL = 0, 1, 2

XTAL          = 24_000_000
OSC_RC_24M    = 24_000_000
OSC_RC_400M   = 400_000_000
SYS_PLL1      = 1_000_000_000
SYS_PLL2      = XTAL * 22            # 528 MHz
SYS_PLL3      = XTAL * 20            # 480 MHz

# (root, mux) -> source Hz.  From fsl_clock.c s_clockSourceName + CLOCK_GetFreq.
# Only the roots this test drives; each is stated INDEPENDENTLY of the model.
CASES = []
for div in (1, 2, 3, 4, 8, 16, 256):                   # DIV field is 8 bits: 1..256
    CASES += [
        # BUS_AON (root 3): mux2 = SysPll2Out.  The EVK runs this at div 4 = 132 MHz.
        (3, 2, div, SYS_PLL2      // div),
        # BUS_AON mux0/mux1: the RC oscillators.
        (3, 0, div, OSC_RC_24M    // div),
        (3, 1, div, OSC_RC_400M   // div),
        # GPT1 (root 19): mux2 = SysPll3Div2.  The EVK runs this at div 1 = 240 MHz.
        (19, 2, div, (SYS_PLL3 // 2) // div),
        # EDGELOCK (root 2): mux2 = SysPll1Out = 1 GHz.
        (2,  2, div, SYS_PLL1     // div),
        # CKO2 (root 73): mux2 = SysPll1Div5 = 200 MHz.  The LAST row of the mux
        # table -- an under-filled table zero-fills to OSC_RC_24M and would read
        # 24 MHz here, a plausible number. This is the row the generator dropped.
        (73, 2, div, (SYS_PLL1 // 5) // div),
    ]


def qtest(script):
    """
    THERE IS NO `quit` IN THE QTEST PROTOCOL -- it answers "FAIL Unknown command".
    QEMU never exits; THE HARNESS MUST KILL IT.  communicate() waits for EOF, so a
    perfectly healthy run hangs until the deadline and reports CANNOT TELL.  (It did
    exactly that on this test's first run, with every answer already correct on the
    wire.)  So: read exactly as many answers as we asked questions, then kill.

    `-accel qtest` is load-bearing.  WITHOUT it the vCPU free-runs a zeroed vector
    table -- a SECOND WRITER to the address space we are reading. (mcxn947qemu)
    """
    p = subprocess.Popen(
        [QEMU, "-M", "mimxrt1180-evk", "-display", "none", "-accel", "qtest",
         "-qtest", "stdio", "-monitor", "none", "-serial", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)

    answers, timed_out = [], False

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
    except BrokenPipeError:
        pass
    t.join(TIMEOUT)
    timed_out = t.is_alive()
    p.kill()
    p.wait()

    if timed_out:
        print("CANNOT TELL: qemu answered %d of %d questions within %ds."
              % (len(answers), len(script), TIMEOUT))
        print("      A wedged model looks BUSY, not BROKEN.  This is NOT a pass and")
        print("      it is NOT a failure -- it is the gate reporting it could not run.")
        sys.exit(EXIT_CANNOT_TELL)

    # ASSERT YOU GOT AS MANY ANSWERS AS YOU ASKED QUESTIONS.  A truncated
    # conversation and a correct one differ only in the answers you never notice
    # are missing.  And `writel` also answers "OK" -- so align answers to commands
    # by POSITION and take only the reads, rather than scraping every OK line
    # (which would hand back the literal string "OK" for a write).
    if len(answers) != len(script):
        print("CANNOT TELL: asked %d questions, got %d answers."
              % (len(script), len(answers)))
        sys.exit(EXIT_CANNOT_TELL)

    return [a.split()[-1] for cmd, a in zip(script, answers)
            if cmd.startswith("readl")]


script, expect = [], []
for root, mux, div, want in CASES:
    ctrl = (mux << 8) | (div - 1)          # DIV field holds (divisor - 1)
    script.append("writel 0x%x 0x%x" % (ROOT_CTRL(root), ctrl))
    script.append("writel 0x%x 0x%x" % (OBS_CTRL(0), root))   # observe that root, /1
    script.append("readl 0x%x" % OBS_FREQ(0))
    expect.append((root, mux, div, want))

vals = qtest(script)
if len(vals) != len(expect):
    print("CANNOT TELL: asked %d questions, got %d answers." % (len(expect), len(vals)))
    print("      A truncated conversation and a correct one differ only in the")
    print("      answers you never notice are missing.")
    sys.exit(EXIT_CANNOT_TELL)

bad = []
for (root, mux, div, want), got in zip(expect, vals):
    got = int(got, 16)
    if got != want:
        bad.append((root, mux, div, want, got))

print("CCM clock tree: %d (root, mux, div) combinations probed" % len(expect))
print("  each verified against a frequency derived from the SDK's OWN constants,")
print("  not from this model.")

if bad:
    print("\nFAIL: %d combination(s) report the wrong frequency." % len(bad))
    for root, mux, div, want, got in bad[:20]:
        print("        root %-2d mux %d div %-3d  want %10d Hz  got %10d Hz  (%.2fx)"
              % (root, mux, div, want, got, (got / want) if want else 0))
    sys.exit(EXIT_LIES)

print("\nPASS: every root reports source/(DIV+1) exactly.")
print("      Bus_Aon  @ mux2 div4 = %d Hz   (the EVK's 132 MHz)" % (SYS_PLL2 // 4))
print("      Gpt1     @ mux2 div1 = %d Hz   (the EVK's 240 MHz)" % (SYS_PLL3 // 2))

# ---------------------------------------------------------------------------
# PHASE 2: CLOCK_GetFreqFromObs(), THROUGH THE SET/CLR ALIASES.
#
# The SDK arms the frequency detector ENTIRELY through CONTROL_SET / CONTROL_CLR.
# This model used to treat those as ORDINARY REGISTERS -- `regs[off/4] = value` --
# so every alias write landed in a backing word nobody reads and THE BASE REGISTER
# NEVER MOVED.  The arming sequence did nothing, and
#
#     while (OBSERVE[i].FREQUENCY_CURRENT == 0UL) { }
#
# would have spun FOREVER.  (91emulator found the identical bug in the i.MX 91 CCM:
# "writes to CONTROL_SET were dropped on the floor" -- and it hid because both read
# paths were right by accident.)
#
# So drive the driver's OWN sequence, verbatim, and require the number it would
# return: 132 MHz.  Reading FREQUENCY_CURRENT while the slice is HELD IN RESET must
# give 0 -- a detector that has not run has not measured anything, which is what the
# RM's reset column says.
# ---------------------------------------------------------------------------
OBS_SEL, OBS_RAW, OBS_RESET, OBS_OFF = 0x1FF, 0x1000, 0x8000, 0x1000000
CCM_OBS_DIV = 3                      # fsl_clock.c
ROOT_BUS_AON = 3

seq = [
    "writel 0x%x 0x%x" % (ROOT_CTRL(ROOT_BUS_AON), (2 << 8) | (4 - 1)),  # 528/4
    "writel 0x%x 0x%x" % (OBS_CTRL(0),        OBS_OFF),        # CONTROL   = OFF
    "writel 0x%x 0x%x" % (OBS_CTRL(0) + 0x4,  OBS_RESET),      # CONTROL_SET = RESET
    "writel 0x%x 0x%x" % (OBS_CTRL(0) + 0x8,  OBS_RAW),        # CONTROL_CLR = RAW
    "writel 0x%x 0x%x" % (OBS_CTRL(0),
                          (OBS_OFF | OBS_RESET) | ROOT_BUS_AON | (CCM_OBS_DIV << 16)),
    "readl 0x%x" % OBS_FREQ(0),                                # held in reset -> 0
    "writel 0x%x 0x%x" % (OBS_CTRL(0) + 0x8, OBS_RESET | OBS_OFF),   # un-reset + START
    "readl 0x%x" % OBS_FREQ(0),                                # now it measures
]
held, running = [int(v, 16) for v in qtest(seq)]
want_obs = (SYS_PLL2 // 4) // (CCM_OBS_DIV + 1)
sdk_would_return = running * (CCM_OBS_DIV + 1)

print("\nCLOCK_GetFreqFromObs(), driven through the SET/CLR aliases:")
print("  FREQUENCY_CURRENT held in reset : %d          (the RM's reset value)" % held)
print("  FREQUENCY_CURRENT running       : %d Hz" % running)
print("  the SDK returns FREQ * (DIV+1)  : %d Hz" % sdk_would_return)

if held != 0 or running != want_obs or sdk_would_return != SYS_PLL2 // 4:
    print("\nFAIL: the observe path is wrong.")
    print("      want held=0, running=%d, returned=%d" % (want_obs, SYS_PLL2 // 4))
    print("      A zero here means the alias writes were DISCARDED and the SDK's")
    print("      `while (FREQUENCY_CURRENT == 0) {}` would spin forever.")
    sys.exit(EXIT_LIES)

print("\nPASS: the driver's own arming sequence terminates and yields 132 MHz.")
sys.exit(EXIT_PASS)
