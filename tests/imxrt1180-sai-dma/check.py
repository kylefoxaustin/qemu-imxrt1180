#!/usr/bin/env python3
# SAI-TX-BY-eDMA oracle.  The guest never writes TDR: the eDMA moves the samples
# from memory into the SAI FIFO on the FIFO's own request line, and they clock out
# to a wav.  As with the PIO SAI test, the VERDICT IS RENDERED OUTSIDE THE GUEST --
# a verdict computed inside it cannot see a FIFO that accepted every DMA write and
# clocked silence.  We render with `-audio driver=wav`, which opens a FILE, never a
# device, so no path through this file reaches a speaker.
#
# TWO OPERATING POINTS, same reason as the PIO test: a model that ignores TCR2[DIV]
# renders both dividers at one rate and mixeng resamples one of them, moving every
# sample.  The byte-exact compare at the pinned rate IS the rate assertion.
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.environ.get("QEMU", os.path.join(HERE, "../../build/qemu-system-arm"))
ELF = os.environ.get("ELF", os.path.join(HERE, "saidma.elf"))

# ── THE GOLDEN, DERIVED FROM THE RM AND THE FIRMWARE'S INTENT (not read back) ──
#   CCM root 65 mux 0 = OSC_RC_24M = 24 MHz, DIV field 0 (divisor 1)  -> MCLK 24e6
#   BCLK = MCLK / (2 * (TCR2[DIV] + 1))
#   rate = BCLK / (FRSZ+1 words * (W0W+1) bits)
MCLK_HZ = 24_000_000
WORDS_PER_FRAME = 2          # TCR4[FRSZ] + 1
BITS_PER_WORD = 16           # TCR5[W0W] + 1
WANT_CHANNELS = 2

def rate_for(div):
    bclk = MCLK_HZ // (2 * (div + 1))
    return bclk // (WORDS_PER_FRAME * BITS_PER_WORD)

SWEEP = [14, 29]             # -> 25000 Hz and 12500 Hz, both exact
NSAMPLES = 2048
def wave_sample(n):
    v = (n * 977) & 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v
GOLDEN = [wave_sample(n) for n in range(NSAMPLES)]

SEL_ADDR = 0x20001000
SEL_MAGIC = 0x53414931       # "SAI1"


def fail(msg):
    print("FAIL: %s" % msg)
    sys.exit(1)


tmp = tempfile.mkdtemp(prefix="saidma-")


def play(div):
    want_rate = rate_for(div)
    wav_path = os.path.join(tmp, "saidma-%d.wav" % div)
    try:
        proc = subprocess.run(
            [QEMU, "-M", "mimxrt1180-evk", "-display", "none", "-monitor", "none",
             "-semihosting-config", "enable=on,target=native",
             "-device", "loader,addr=0x%x,data=0x%x,data-len=4" % (SEL_ADDR, SEL_MAGIC),
             "-device", "loader,addr=0x%x,data=%d,data-len=4" % (SEL_ADDR + 4, div),
             # Pin the backend to the derived rate: mixeng passes samples through
             # untouched ONLY if the model's voice is genuinely at that rate, so the
             # byte-exact compare below is also the rate assertion.
             "-audio", "driver=wav,id=sai,path=%s,out.frequency=%d,out.channels=%d,"
                       "out.format=s16" % (wav_path, want_rate, WANT_CHANNELS),
             "-kernel", ELF, "-serial", "stdio"],
            capture_output=True, text=True, timeout=90, check=False)
        console = proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as e:
        console = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        fail("DIV=%d: the guest never finished -- the eDMA-fed FIFO likely never\n"
             "      completed its major loop (DMA request line never serviced?).\n"
             "      last console:\n      %s" % (div, console.strip()[-400:]))

    # The guest's OWN verdict covers the register-level gates (FRDE refuses; ERQ
    # clears at completion).  A FAIL here is a real defect in the request path.
    if "SAIDMA: FAIL" in console:
        fail("DIV=%d: the guest reported a gate failure:\n      %s"
             % (div, [l for l in console.splitlines() if "FAIL" in l][-1]))
    if "SAIDMA: PASS" not in console:
        fail("DIV=%d: the guest did not reach its PASS line; console:\n      %s"
             % (div, console.strip()[-400:]))
    if not os.path.exists(wav_path) or os.path.getsize(wav_path) == 0:
        fail("DIV=%d: the eDMA-fed SAI produced NO AUDIO AT ALL.\n"
             "      The channel completed but the backend wrote nothing -- what a TDR\n"
             "      that accepts-and-discards looks like from outside the guest." % div)

    # Parse the RIFF by hand: the guest exits via semihosting, so QEMU never runs the
    # wav finaliser and the RIFF/data lengths stay ZERO.  A MISSING WITNESS AND A
    # SILENT ONE PRODUCE THE SAME NUMBER (91emulator) -- so read the bytes directly.
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
        fail("DIV=%d: THE eDMA-FED SAI CLOCKED PURE SILENCE: %d samples, ALL ZERO.\n"
             "      The DMA 'moved' words and the guest passed its gate checks, yet nothing\n"
             "      real reached the sink -- exactly what an in-guest oracle cannot notice."
             % (div, len(samples)))
    if len(samples) < NSAMPLES:
        fail("DIV=%d (%d Hz): the wav holds %d samples; the eDMA moved %d.\n"
             "      Either the SAI dropped %d, OR its voice is at a rate that is not %d Hz\n"
             "      (mixeng resampled and the count changed). A model that IGNORES TCR2[DIV]\n"
             "      fails here at the SECOND operating point and nowhere else."
             % (div, want_rate, len(samples), NSAMPLES, NSAMPLES - len(samples), want_rate))
    mismatch = [(i, GOLDEN[i], samples[i])
                for i in range(NSAMPLES) if samples[i] != GOLDEN[i]]
    if mismatch:
        i, want, got = mismatch[0]
        fail("DIV=%d (%d Hz): THE SAMPLES ARE NOT THE ONES THE eDMA MOVED.\n"
             "      %d of %d differ; first at index %d: source %d, played %d\n"
             "      Either the bytes were altered crossing block+DMA, or the voice is at the\n"
             "      wrong rate and mixeng interpolated them. A RANGE IS NOT A GOLDEN."
             % (div, want_rate, len(mismatch), NSAMPLES, i, want, got))

    print("  ok  DIV=%-2d -> %5d Hz: %d samples via eDMA, byte-exact, peak %d, %d non-zero"
          % (div, want_rate, len(samples), peak, nonzero))

print("\nPASS: the SAI's TX FIFO asserts its DMA request line, the eDMA services it, and the\n"
      "      samples it moved from memory reach the sink UNALTERED at the rate the SAI's own\n"
      "      registers describe -- AT TWO OPERATING POINTS, and the CPU never touched TDR.")
sys.exit(0)
