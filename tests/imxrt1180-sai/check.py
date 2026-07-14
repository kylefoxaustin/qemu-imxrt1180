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
TCR2_DIV = 14
WORDS_PER_FRAME = 2          # TCR4[FRSZ] + 1
BITS_PER_WORD = 16           # TCR5[W0W] + 1
BCLK_HZ = MCLK_HZ // (2 * (TCR2_DIV + 1))
WANT_RATE = BCLK_HZ // (WORDS_PER_FRAME * BITS_PER_WORD)   # == 25000
WANT_CHANNELS = 2

NSAMPLES = 4096
def wave_sample(n):
    """The firmware's waveform, recomputed here. Same three lines, no shared code."""
    v = (n * 977) & 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v

GOLDEN = [wave_sample(n) for n in range(NSAMPLES)]


def fail(msg):
    print("FAIL: %s" % msg)
    sys.exit(1)


tmp = tempfile.mkdtemp(prefix="sai-")
wav_path = os.path.join(tmp, "sai.wav")

proc = subprocess.run(
    [QEMU, "-M", "mimxrt1180-evk", "-display", "none", "-monitor", "none",
     "-semihosting-config", "enable=on,target=native",
     # PIN THE BACKEND TO THE RATE THE GUEST PROGRAMMED, and this is load-bearing.
     #
     # QEMU's mixeng RESAMPLES between the device's voice and the backend. Left at the
     # wav driver's default 44100, our 25000 Hz voice was resampled UP -- and a
     # resampled stream can never be byte-compared against what the firmware wrote.
     #
     # Pinned to 25000, mixeng passes samples through UNTOUCHED *only if the model's
     # voice is genuinely at 25000 too*. So the byte-exact assertion below IS the rate
     # assertion: open the voice at the wrong rate and mixeng interpolates, and every
     # sample moves. (Mutation-proven: hardcode 48000 in the model and this fails.)
     "-audio", "driver=wav,id=sai,path=%s,out.frequency=%d,out.channels=%d,"
               "out.format=s16" % (wav_path, WANT_RATE, WANT_CHANNELS),
     "-kernel", ELF, "-serial", "stdio"],
    capture_output=True, text=True, timeout=180)
console = proc.stdout + proc.stderr

print("  guest said: %s" % (console.strip().splitlines() or ["<nothing>"])[-1])

if "SAI: FAIL" in console:
    fail("the firmware's own init check failed:\n      %s" % console.strip())
if "FIFO ERROR" in console:
    fail("the SAI reported FEF (over/underrun) during playback:\n      %s"
         % console.strip())

# ── THE ASSERTION: THE BYTES ─────────────────────────────────────────────────
if not os.path.exists(wav_path) or os.path.getsize(wav_path) == 0:
    fail("the SAI produced NO AUDIO AT ALL.\n"
         "      The guest completed its writes and the wav backend wrote nothing —\n"
         "      which is exactly what a TDR that accepts and discards looks like from\n"
         "      the outside, and exactly what the old handshake test could not see.")

# PARSE THE RIFF BY HAND.
#
# The guest exits via semihosting, so QEMU never runs the wav backend's finaliser and
# the RIFF/data length fields stay ZERO. Python's `wave` module refuses the file
# outright ("not a WAVE file") -- and a checker that trusted it would have reported a
# BROKEN MODEL when the model was fine and the bytes were all there.
#
#   ⭐ A MISSING WITNESS AND A SILENT ONE PRODUCE THE SAME NUMBER. (91emulator)
#      Read the bytes that ARE there; do not ask a parser whether a length field it
#      does not need was filled in.
raw = open(wav_path, "rb").read()
if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
    fail("the audio backend produced something that is not a RIFF/WAVE file")

fi = raw.index(b"fmt ") + 8
channels, rate = struct.unpack("<HI", raw[fi + 2:fi + 8])
width = struct.unpack("<H", raw[fi + 14:fi + 16])[0] // 8
pcm = raw[raw.index(b"data") + 8:]

if width != 2:
    fail("expected 16-bit samples, got %d-bit" % (width * 8))

# 1. The container agrees with what we asked the backend for. (This alone proves
#    nothing about the MODEL -- we pinned it. The samples below are what prove that.)
if rate != WANT_RATE:
    fail("SAMPLE RATE IS WRONG.\n"
         "      wav says     : %d Hz\n"
         "      RM says      : %d Hz  (MCLK %d / (2*(DIV+1)) / (%d words * %d bits))\n\n"
         "      This golden is derived from the RM's formula and the registers the\n"
         "      FIRMWARE wrote — not from the model. A hardcoded 48000 would pass a\n"
         "      model whose clock tree was never wired up at all."
         % (rate, WANT_RATE, MCLK_HZ, WORDS_PER_FRAME, BITS_PER_WORD))

if channels != WANT_CHANNELS:
    fail("expected %d channels (TCR4[FRSZ]+1), wav says %d"
         % (WANT_CHANNELS, channels))

samples = list(struct.unpack("<%dh" % (len(pcm) // 2), pcm))

# 2. IT IS NOT SILENCE.  This is the assertion 91's mutation defeated on their tree:
#    the guest's oracle passed while every sample was zero.
nonzero = sum(1 for s in samples if s != 0)
peak = max((abs(s) for s in samples), default=0)
if nonzero == 0:
    fail("THE SAI CLOCKED PURE SILENCE: %d samples, ALL ZERO, peak 0.\n"
         "      The guest would have reported success anyway — this is precisely the\n"
         "      failure 91emulator's memset mutation produced, and precisely what an\n"
         "      in-guest oracle is structurally incapable of noticing." % len(samples))

# 3. AND THEY ARE THE RIGHT SAMPLES, IN THE RIGHT ORDER.
#    "Non-silent" is not a golden — a 3x-wrong or byte-swapped stream is non-silent
#    too.  ⭐ A RANGE IS NOT A GOLDEN.  So compare against the waveform the firmware
#    INTENDED, recomputed here from first principles.
if len(samples) < NSAMPLES:
    fail("the wav holds %d samples; the firmware wrote %d.\n"
         "      Either the SAI dropped %d of them, OR its voice is open at a rate that\n"
         "      is not %d Hz -- in which case mixeng RESAMPLED the stream and the count\n"
         "      changed on the way out. Both are real defects; check the model's\n"
         "      imxrt1180_sai_tx_hz() against the RM's BCLK formula."
         % (len(samples), NSAMPLES, NSAMPLES - len(samples), WANT_RATE))

mismatch = [(i, GOLDEN[i], samples[i])
            for i in range(NSAMPLES) if samples[i] != GOLDEN[i]]
if mismatch:
    i, want, got = mismatch[0]
    fail("THE SAMPLES ARE NOT THE SAMPLES THE FIRMWARE WROTE.\n"
         "      %d of %d differ; first at index %d: wrote %d, played %d\n\n"
         "      The bytes crossed the block and came out CHANGED — a packing, ordering\n"
         "      or width bug. Non-silence would have hidden every one of these."
         % (len(mismatch), NSAMPLES, i, want, got))

print("  ok  wav: %d Hz, %d ch, %d samples (firmware wrote %d)"
      % (rate, channels, len(samples), NSAMPLES))
print("  ok  NOT silence: %d non-zero, peak %d" % (nonzero, peak))
print("  ok  all %d samples byte-exact against the waveform the firmware intended"
      % NSAMPLES)
print("\nPASS: the SAI moved the guest's samples to a sink, at the rate its own "
      "registers\n      describe, unaltered — and the verdict was rendered outside "
      "the guest.")
sys.exit(0)
