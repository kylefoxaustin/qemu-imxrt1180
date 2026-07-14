#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# THE SAI ORACLE, AND IT LIVES OUTSIDE THE GUEST.
#
# ─────────────────────────────────────────────────────────────────────────────
# The old test was the firmware saying "PASS - TX init handshake settles" while the
# model did this:
#
#     case SAI_TDR0:  /* Transmit data is accepted and discarded */  return;
#
# The block could swallow every sample the firmware ever wrote and emit NOTHING, and
# the suite reported "sai: pass" — for months, in every run I quoted to the fleet.
#
# 91emulator proved the general case on their own SAI, and it is the reason this file
# exists: they made the SAI clock PURE SILENCE, and the guest's own ALSA oracle STILL
# REPORTED PASS.  snd_pcm_writei() and drain() succeed perfectly well against a device
# that is faithfully clocking zeros.
#
#   ⭐ THE ORACLE'S WORD IS NOT THE ORACLE.  A verdict computed INSIDE the guest cannot
#      tell a working device from one that accepted every write and produced nothing.
#      Only something outside it, LOOKING AT THE SAMPLES, can.
#
#   ⭐ AND A TEST WHOSE VERDICT IS READ BY A HUMAN IS NOT A TEST.  (91emulator, on
#      having hand-grepped their own terminal for a week: "I WAS THE ASSERTION.")
#      This exits 0 or 1.
#
# THE MUTE IS STRUCTURAL, NOT REMEMBERED.  We render with `-audio driver=wav`, which
# opens a FILE and never a device — so the capture IS the mute and IS the evidence,
# and the test cannot render a verdict without it.  There is no path through this file
# that reaches a speaker.
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.environ.get("QEMU", os.path.join(HERE, "../../build/qemu-system-arm"))
ELF = os.environ.get("ELF", os.path.join(HERE, "sai.elf"))

# ── THE GOLDEN, DERIVED FROM THE RM AND THE FIRMWARE'S OWN INTENT ─────────────
#
# NOT read back from the model.  tests/imxrt1180-pwm asserted a carrier period against
# a PWM_HZ it had taken FROM the model, said so in a comment, and passed a clock that
# was 1.5x wrong:  ⭐ A MIRROR THAT DECLARES ITSELF IS STILL A MIRROR.
#
#   CCM root 65 mux 0 = OSC_RC_24M = 24 MHz, DIV field 0 (divisor 1)  -> MCLK 24e6
#   BCLK = MCLK / (2 * (TCR2[DIV] + 1)),  firmware writes DIV = 14    -> 800 000
#   rate = BCLK / (FRSZ+1 words * (W0W+1) bits) = 800000 / (2 * 16)   ->  25 000
MCLK_HZ = 24_000_000
WORDS_PER_FRAME = 2          # TCR4[FRSZ] + 1
BITS_PER_WORD = 16           # TCR5[W0W] + 1
WANT_CHANNELS = 2

def rate_for(div):
    """The RM's formula, for a bit-clock MASTER. rate = MCLK / (2*(DIV+1)) / (words*bits)."""
    bclk = MCLK_HZ // (2 * (div + 1))
    return bclk // (WORDS_PER_FRAME * BITS_PER_WORD)

# ⭐ TWO OPERATING POINTS. ONE IS NOT A TEST.
#
# 93emulator, 2026-07-14, who shipped the same divider derivation and then retracted it:
#
#   "It is the correct formula. It is in the RM. It produces exactly 48000 at 48 kHz. AND IT
#    IS WRONG. ... A FORMULA THAT IS CORRECT AT THE POINT YOU TESTED IT IS NOT A FORMULA YOU
#    HAVE TESTED. Only a SECOND operating point exposed mine."
#
# Their SAI ignored TCR2[DIV] completely and every audio test in the fleet asked for 48 kHz --
# the one rate that cannot see it. This test asked for 25 kHz, once, and had exactly the same
# blind spot: a model that ignored the divider would have reproduced our golden perfectly.
#
# So the divider is now an ARGUMENT, poked into the guest by the harness, and we sweep it.
# A rate-ignoring model transposes the second point and every sample moves.
SWEEP = [14, 29]             # -> 25000 Hz and 12500 Hz, both exact, no rounding

NSAMPLES = 4096
def wave_sample(n):
    """The firmware's waveform, recomputed here. Same three lines, no shared code."""
    v = (n * 977) & 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v

GOLDEN = [wave_sample(n) for n in range(NSAMPLES)]

SEL_ADDR = 0x20001000        # {magic, div} -- the firmware REFUSES to run without it
SEL_MAGIC = 0x53414931       # "SAI1"


def fail(msg):
    print("FAIL: %s" % msg)
    sys.exit(1)


tmp = tempfile.mkdtemp(prefix="sai-")


def play(div):
    """Run the SAI at one operating point and return the samples it actually emitted."""
    want_rate = rate_for(div)
    wav_path = os.path.join(tmp, "sai-%d.wav" % div)
    stalled = False

    # A STALLED SAI IS A REAL OUTCOME, NOT A HARNESS BUG -- SO NAME IT.
    #
    # If the model cannot determine a rate (e.g. TCR2[BCD]=0, a bit-clock SLAVE with no codec
    # modelled), it opens no voice, the FIFO never drains, and the guest spins forever on FRF
    # waiting for a slot. THAT IS WHAT THE SILICON DOES: no bit clock, no transmission, a
    # driver that waits forever. It is faithful, and it is diagnosable in a minute.
    #
    # What it must never do is invent a rate so the wait "succeeds" -- a SAI running at a
    # plausible-but-wrong rate ships. So a timeout here is reported AS a stall, not as an
    # infrastructure failure that someone will paper over with a bigger number.
    try:
        proc = subprocess.run(
            [QEMU, "-M", "mimxrt1180-evk", "-display", "none", "-monitor", "none",
             "-semihosting-config", "enable=on,target=native",
             # ARM THE GUEST. The firmware FAILS if this slot is not set -- a test that
             # silently supplies its own input is testing itself.
             "-device", "loader,addr=0x%x,data=0x%x,data-len=4" % (SEL_ADDR, SEL_MAGIC),
             "-device", "loader,addr=0x%x,data=%d,data-len=4" % (SEL_ADDR + 4, div),
             # PIN THE BACKEND TO THE RATE THE GUEST PROGRAMMED, and this is load-bearing.
             #
             # QEMU's mixeng RESAMPLES between the device's voice and the backend, and the wav
             # driver's own default is 44100. 95emulator tried to read the rate out of the wav
             # HEADER and got 0 Hz for a CORRECT model:
             #
             #   ⭐ "AN ORACLE THAT MEASURES THE WRONG DEVICE IS WORSE THAN NO ORACLE. IT IS A
             #      GREEN LIGHT WITH A CITATION." -- the header describes the BACKEND, not the SAI.
             #
             # Pinned to the derived rate, mixeng passes samples through UNTOUCHED *only if the
             # model's voice is genuinely at that rate*. So the byte-exact comparison below IS
             # the rate assertion -- open the voice at the wrong rate and every sample moves.
             "-audio", "driver=wav,id=sai,path=%s,out.frequency=%d,out.channels=%d,"
                       "out.format=s16" % (wav_path, want_rate, WANT_CHANNELS),
             "-kernel", ELF, "-serial", "stdio"],
            capture_output=True, text=True, timeout=90, check=False)
        console = proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as e:
        # THE GUEST IS STILL WAITING FOR A SLOT THAT WILL NEVER COME.
        console = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        stalled = True

    if stalled:
        fail("DIV=%d: THE SAI NEVER CLOCKED A SAMPLE -- the guest is still spinning on\n"
             "      FRF waiting for FIFO space. The model opened no voice, so nothing\n"
             "      drains.\n\n"
             "      That is what a bit-clock SLAVE with no codec does on silicon, and the\n"
             "      model is right to refuse rather than invent a rate. But it means the\n"
             "      firmware asked for a configuration this machine cannot render.\n"
             "      Check TCR2[BCD]: if it is 0, the rate is NOT in the SAI." % div)

    if "SAI: FAIL" in console:
        fail("DIV=%d: the firmware refused to run:\n      %s"
             % (div, console.strip().splitlines()[-1]))
    if "FIFO ERROR" in console:
        fail("DIV=%d: the SAI reported FEF (over/underrun) during playback" % div)
    if not os.path.exists(wav_path) or os.path.getsize(wav_path) == 0:
        fail("DIV=%d: the SAI produced NO AUDIO AT ALL.\n"
             "      The guest completed its writes and the backend wrote nothing -- which is\n"
             "      what a TDR that accepts and discards looks like from outside, and what a\n"
             "      register-handshake test could never see." % div)

    # PARSE THE RIFF BY HAND: the guest exits via semihosting, so QEMU never runs the wav
    # finaliser and the RIFF/data lengths stay ZERO. Python's `wave` module refuses the file
    # outright -- and a checker that trusted it would report a BROKEN MODEL when the model is
    # fine and every byte is present.
    #   ⭐ A MISSING WITNESS AND A SILENT ONE PRODUCE THE SAME NUMBER. (91emulator)
    raw = open(wav_path, "rb").read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        fail("DIV=%d: the audio backend produced something that is not a RIFF/WAVE file" % div)
    fi = raw.index(b"fmt ") + 8
    channels, rate = struct.unpack("<HI", raw[fi + 2:fi + 8])
    width = struct.unpack("<H", raw[fi + 14:fi + 16])[0] // 8
    pcm = raw[raw.index(b"data") + 8:]

    if width != 2:
        fail("DIV=%d: expected 16-bit samples, got %d-bit" % (div, width * 8))
    if channels != WANT_CHANNELS:
        fail("DIV=%d: expected %d channels, wav says %d" % (div, WANT_CHANNELS, channels))
    return rate, list(struct.unpack("<%dh" % (len(pcm) // 2), pcm))


for div in SWEEP:
    want_rate = rate_for(div)
    rate, samples = play(div)

    nonzero = sum(1 for x in samples if x != 0)
    peak = max((abs(x) for x in samples), default=0)
    if nonzero == 0:
        fail("DIV=%d: THE SAI CLOCKED PURE SILENCE: %d samples, ALL ZERO.\n"
             "      The guest would have reported success anyway -- this is exactly the\n"
             "      failure 91emulator's memset mutation produced, and exactly what an\n"
             "      in-guest oracle is structurally incapable of noticing."
             % (div, len(samples)))

    if len(samples) < NSAMPLES:
        fail("DIV=%d (%d Hz): the wav holds %d samples; the firmware wrote %d.\n"
             "      Either the SAI dropped %d of them, OR its voice is open at a rate that is\n"
             "      not %d Hz -- in which case mixeng RESAMPLED the stream and the count\n"
             "      changed on the way out. A model that IGNORES TCR2[DIV] fails here at the\n"
             "      SECOND operating point and nowhere else."
             % (div, want_rate, len(samples), NSAMPLES, NSAMPLES - len(samples), want_rate))

    mismatch = [(i, GOLDEN[i], samples[i])
                for i in range(NSAMPLES) if samples[i] != GOLDEN[i]]
    if mismatch:
        i, want, got = mismatch[0]
        fail("DIV=%d (%d Hz): THE SAMPLES ARE NOT THE SAMPLES THE FIRMWARE WROTE.\n"
             "      %d of %d differ; first at index %d: wrote %d, played %d\n\n"
             "      Either the bytes were altered crossing the block, or the voice is at the\n"
             "      wrong rate and mixeng interpolated them. 'Non-silent' would have hidden\n"
             "      every one of these: A RANGE IS NOT A GOLDEN."
             % (div, want_rate, len(mismatch), NSAMPLES, i, want, got))

    print("  ok  DIV=%-2d -> %5d Hz: %d samples, byte-exact, peak %d, %d non-zero"
          % (div, want_rate, len(samples), peak, nonzero))

print("\n  the two points are %d Hz apart (%.1fx). A model that ignores TCR2[DIV] renders\n"
      "  BOTH at the same rate -- and mixeng then resamples one of them, moving every sample."
      % (rate_for(SWEEP[0]) - rate_for(SWEEP[1]),
         rate_for(SWEEP[0]) / rate_for(SWEEP[1])))
print("\nPASS: the SAI moves the guest's samples to a sink, unaltered, at the rate its own\n"
      "      registers describe -- AT TWO OPERATING POINTS -- and the verdict was rendered\n"
      "      outside the guest.")
sys.exit(0)
