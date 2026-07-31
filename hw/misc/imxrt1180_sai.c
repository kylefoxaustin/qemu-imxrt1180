/*
 * NXP i.MX RT1180 SAI (Serial Audio Interface / I2S).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT THIS USED TO BE, AND WHY IT WAS GREEN
 *
 * The transmit data register did this:
 *
 *     case SAI_TDR0:
 *         /​* Transmit data is accepted and discarded (no audio sink modelled). *​/
 *         return;
 *
 * ...and the only test we had asserted that TCSR's reset bit self-cleared:
 *
 *     "SAI: PASS - TX init handshake settles (reset self-clear + FWF)"
 *
 * So the block could accept every sample the firmware ever wrote, emit NOTHING,
 * and the suite stayed green -- and I reported "sai: pass" in every suite run.
 *
 * 91emulator proved the general case on their own SAI, and it is sharper than
 * "we had no sink": they memset their capture ring so the SAI clocked PURE
 * SILENCE, and the guest's own ALSA oracle STILL REPORTED PASS.  snd_pcm_writei()
 * and drain() succeed perfectly well against a device that is faithfully clocking
 * zeros.  The guest cannot hear itself.
 *
 *   ⭐ THE ORACLE'S WORD IS NOT THE ORACLE.  A verdict computed INSIDE the guest
 *      cannot distinguish a working device from one that accepted every write and
 *      produced nothing.  Only something OUTSIDE it, looking at the SAMPLES, can.
 *
 * So the TX path is now real: TDR words land in a FIFO, drain at the rate the
 * guest's OWN REGISTERS describe, and go to QEMU's audio backend.  Under
 * `-audio driver=wav,path=out.wav` the bytes the firmware wrote land in a file
 * and become assertable -- which also makes the capture the MUTE (a wav backend
 * opens a FILE, never a device), so the safe path is no longer the one you have
 * to remember.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * AND PARAM WAS A FABRICATION WITH A COMMENT THAT HAD ALREADY DRIFTED FROM IT
 *
 *     #define SAI_PARAM_VALUE  0x00050302u   /​* FIFO=32, channels=2 (best-effort) *​/
 *
 * PARAM[11:8] is log2 of the FIFO depth.  0x3 is EIGHT.  The comment said 32.
 * The silicon says SIXTEEN (SAI1).  Three numbers, no two of them equal -- and
 * the same copy-pasted constant mcxn947qemu found in imx93's SAI ("the comment
 * had already drifted from the value it described, and nobody noticed for
 * months").  It was also ONE value for FOUR DIFFERENT INSTANCES.
 *
 * The RM prints them, per instance (SAI chapter, "Register reset values"):
 *
 *     PARAM   SAI1:      0005_0402h    FRAME=2^5 slots, FIFO=2^4=16, DATALINE=2
 *             SAI2,SAI3: 0005_0501h    FIFO=2^5=32,  DATALINE=1
 *             SAI4:      0005_0504h    FIFO=2^5=32,  DATALINE=4
 *
 * confirmed against FSL_FEATURE_SAI_FIFO_COUNTn() / _CHANNEL_COUNTn(), which the
 * SDK driver is compiled against.  PARAM is now a per-instance property the SoC
 * supplies, and the FIFO the guest gets is the one PARAM promised it.
 *
 *   ⭐ AND THE RESET-VALUE GATE NEVER SAW ANY OF IT: the RM's register table says
 *      PARAM's reset is "See section", so the extractor (correctly) refuses it --
 *      and the actual values are printed two pages later.  A REFUSAL IS NOT A
 *      CHECK.  The gate was green about this register by never looking at it.
 *
 * Offsets/bits from the MIMXRT1189 CMSIS header (I2S_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/misc/imxrt1180_sai.h"
#include "hw/misc/imxrt1180_ccm.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS I2S_Type). */
#define SAI_VERID   0x00    /* RO */
#define SAI_PARAM   0x04    /* RO */
#define SAI_TCSR    0x08    /* Transmit Control/Status */
#define SAI_TCR1    0x0C
#define SAI_TCR2    0x10
#define SAI_TCR3    0x14
#define SAI_TCR4    0x18
#define SAI_TCR5    0x1C
#define SAI_TDR0    0x20    /* TDR[2] @0x20..0x24, WO */
#define SAI_TFR0    0x40    /* TFR[2] @0x40..0x44, RO */
#define SAI_TMR     0x60
#define SAI_RCSR    0x88    /* Receive Control/Status */
#define SAI_RCR1    0x8C
#define SAI_RCR2    0x90
#define SAI_RCR3    0x94
#define SAI_RCR4    0x98
#define SAI_RCR5    0x9C
#define SAI_RDR0    0xA0    /* RDR[2] @0xA0..0xA4, RO */
#define SAI_RFR0    0xC0    /* RFR[2] @0xC0..0xC4, RO */
#define SAI_RMR     0xE0
#define SAI_MCR     0x100

/* TCSR/RCSR bit masks (shared layout for the two CSR registers). */
#define CSR_TE      (1u << 31)  /* transmitter/receiver enable */
#define CSR_FRDE    (1u << 0)   /* FIFO Request DMA Enable (pairs with FRF) */
#define CSR_FWDE    (1u << 1)   /* FIFO Warning DMA Enable (pairs with FWF) */
#define CSR_FRF     (1u << 16)  /* FIFO request flag (reached watermark) */
#define CSR_FWF     (1u << 17)  /* FIFO warning flag (FIFO empty)        */
#define CSR_FEF     (1u << 18)  /* FIFO error (underrun/overrun) flag */
#define CSR_SEF     (1u << 19)  /* sync error flag          */
#define CSR_WSF     (1u << 20)  /* word start flag          */
#define CSR_SR      (1u << 24)  /* software reset           */
#define CSR_FR      (1u << 25)  /* FIFO reset               */
#define CSR_BCE     (1u << 28)  /* bit clock enable         */
#define CSR_EN      (1u << 31)  /* TE for TCSR / RE for RCSR */

#define CSR_FLAGS_W1C  (CSR_FEF | CSR_SEF | CSR_WSF)
#define CSR_IE_TO_FLAG_SHIFT  8
#define CSR_STICKY_FLAGS  (CSR_FEF | CSR_SEF | CSR_WSF)

/* Field accessors (CMSIS I2S_Type). */
#define TCR1_TFW(v)   ((v) & 0x1Fu)                 /* FIFO watermark            */
#define TCR2_DIV(v)   ((v) & 0xFFu)                 /* bit clock divider         */
#define TCR2_BCD(v)   (((v) >> 24) & 0x1u)          /* 1 = bit clock MASTER      */
#define TCR4_FRSZ(v)  (((v) >> 16) & 0x1Fu)         /* frame size - 1 (in words) */
#define TCR5_W0W(v)   (((v) >> 16) & 0x1Fu)         /* word 0 width - 1 (bits)   */
#define TFR_RFP_SHIFT 0
#define TFR_WFP_SHIFT 16

/* PARAM fields (RM: "The number of words in each FIFO is 2^FIFO."). */
#define PARAM_DATALINE(v)  ((v) & 0xFu)
#define PARAM_FIFO(v)      (((v) >> 8) & 0xFu)

/* VERID: the RM prints 0301_0000h for every SAI instance. */
#define SAI_VERID_VALUE  0x03010000u

/*
 * THE SAMPLE RATE IS COMPUTED FROM THE GUEST'S OWN REGISTERS. IT IS NOT 48000.
 *
 *     BCLK = MCLK / (2 * (TCR2[DIV] + 1))
 *     bits per frame = (TCR4[FRSZ] + 1) words * (TCR5[W0W] + 1) bits
 *     sample rate    = BCLK / bits per frame
 *
 * Returns 0 if the guest has not programmed a usable clock -- and 0 MEANS 0.  We
 * do NOT fall back to a plausible 48 kHz:
 *
 *   ⭐ "A ?: IS NOT A SAFETY NET -- IT IS A PLACE FOR A BUG TO LIVE WHERE NO TEST
 *      WILL LOOK."  Six timer blocks in this tree opened with `if (!clk) clk =
 *      DEFAULT;` and not one of the six defaults was right.  A silent SAI is
 *      diagnosed in a minute.  A SAI running at a plausible-but-wrong rate ships.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * ⭐ AND THE FORMULA ABOVE IS ONLY TRUE IF THIS SAI IS THE BIT-CLOCK *MASTER*.
 *
 * 93emulator, 2026-07-14, who shipped exactly this derivation and then retracted it:
 *
 *   "It is the correct formula. It is in the RM. It produces exactly 48000 at 48 kHz.
 *    AND IT IS WRONG.  On a wm8962 EVK the SAI is a bit-clock SLAVE -- the CODEC drives
 *    BCLK/LRCLK, the rate is set in the codec over I2C, and IT IS NOT DERIVABLE FROM ANY
 *    SAI REGISTER.  The information is not in that device.  CHECK TCR2 bit24 BEFORE YOU
 *    WRITE ONE LINE OF DIVIDER MATH."
 *
 * Their driver programmed IDENTICAL TCR2/TCR4 for 48 kHz and 16 kHz -- it never touched
 * the divider, because in slave mode the divider does nothing.  A master-mode formula
 * applied to a slave is A FABRICATION WITH AN RM CITATION ATTACHED, and it agrees with
 * itself at the one operating point anybody tests.
 *
 *   ⭐ A FORMULA THAT IS CORRECT AT THE POINT YOU TESTED IT IS NOT A FORMULA YOU HAVE
 *      TESTED.  Only a SECOND operating point exposes it.
 *
 * So: TCR2[BCD] == 0 (slave) => WE DO NOT KNOW THE RATE, and we say so.  We do not invent
 * one from a divider this block is not driving.  (We model no codec, so on this machine a
 * slave SAI genuinely has no rate source -- the honest answer is "none", not "48000".)
 */
static uint32_t imxrt1180_sai_tx_hz(IMXRT1180SAIState *s, uint32_t *nchan,
                                    uint32_t *word_bits)
{
    uint32_t tcr2 = s->regs[SAI_TCR2 >> 2];
    uint32_t tcr4 = s->regs[SAI_TCR4 >> 2];
    uint32_t tcr5 = s->regs[SAI_TCR5 >> 2];
    uint32_t words = TCR4_FRSZ(tcr4) + 1u;
    uint32_t bits = TCR5_W0W(tcr5) + 1u;
    uint32_t mclk, bclk;

    /*
     * BIT-CLOCK DIRECTION FIRST. Everything below divides MCLK by TCR2[DIV] -- which is
     * only what the hardware does when THIS block generates the bit clock.
     */
    if (!TCR2_BCD(tcr2)) {
        qemu_log_mask(LOG_UNIMP, "imxrt1180-sai: TCR2[BCD]=0, the SAI is a bit-clock "
                      "SLAVE -- the rate is driven by an external codec and is NOT "
                      "derivable from any SAI register. No codec is modelled, so no "
                      "audio is rendered. (Computing MCLK/(2*(DIV+1)) here would be a "
                      "fabrication: in slave mode the SAI does not drive that divider.)\n");
        return 0;
    }

    mclk = imxrt1180_ccm_periph_hz(s->ccm, s->clk_root, "imxrt1180-sai");
    if (!mclk || !words || !bits) {
        return 0;
    }
    bclk = mclk / (2u * (TCR2_DIV(tcr2) + 1u));
    if (!bclk || words * bits == 0u) {
        return 0;
    }
    *nchan = words;
    *word_bits = bits;
    return bclk / (words * bits);
}

static void imxrt1180_sai_update_irq(IMXRT1180SAIState *s);

/*
 * Drain up to `budget_bytes` worth of the TX FIFO into the audio backend, and
 * return the number of WORDS that actually left.  This is the only place samples
 * leave the model, and it is the reason the wav file has anything in it.
 *
 * ⭐ audio_be_write() MAY ACCEPT FEWER BYTES THAN OFFERED, and the number it
 * accepts is NOT bounded by the `budget_bytes` we were handed -- that is the
 * mixeng buffer's free space, not a promise about the voice's write path.  This
 * code used to advance tx_rptr/tx_count for every sample it COPIED and then
 * ignore the return value, so any sample the backend declined was gone from the
 * FIFO forever.  It stayed hidden only because the OLD (wrong) FRF kept the FIFO
 * near-full, so the drain size happened to match what the backend took; once FRF
 * was corrected the FIFO ran at the watermark, the drain size no longer matched,
 * and 456 of 4096 samples vanished -- byte-exactly, every run.
 *
 * So: COMMIT ONLY WHAT WAS ACCEPTED.  A sample the backend did not take stays in
 * the FIFO -- which is exactly "the codec has not consumed it yet", the honest
 * model of a full downstream (and the backend's own RateCtl paces this to fs).
 */
static size_t imxrt1180_sai_drain(IMXRT1180SAIState *s, int budget_bytes)
{
    int16_t buf[IMXRT1180_SAI_FIFO_MAX];
    size_t n = 0, accepted = 0;

    /* PEEK up to the budget -- do NOT advance the read pointer yet. */
    while (n < ARRAY_SIZE(buf) &&
           (int)((n + 1) * sizeof(int16_t)) <= budget_bytes && n < s->tx_count) {
        /*
         * A 32-bit FIFO word carries one word of audio.  For the 16-bit case the
         * SDK writes the sample right-justified, so the low half IS the sample.
         */
        buf[n] = (int16_t)(s->tx_fifo[(s->tx_rptr + n) % IMXRT1180_SAI_FIFO_MAX]
                           & 0xFFFFu);
        n++;
    }

    if (n) {
        int wrote = audio_be_write(s->audio_be, s->voice, buf, n * sizeof(int16_t));
        accepted = (wrote > 0) ? (size_t)wrote / sizeof(int16_t) : 0;
        s->tx_rptr = (s->tx_rptr + accepted) % IMXRT1180_SAI_FIFO_MAX;
        s->tx_count -= accepted;
    }
    return accepted;
}

/*
 * The audio backend's SW-voice callback fires at the audiodev timer rate when the
 * mixeng has free space.  The FIFO drain is owned by the fs-paced drain_timer
 * below (the codec's real pull), NOT by this callback -- draining here too would
 * empty the FIFO faster than fs, spuriously tripping FEF and letting the guest
 * outrun the backend's wall-clock pacing (truncating a wav).  So this is a no-op;
 * audio_be_write() from the timer feeds the backend directly.
 */
static void imxrt1180_sai_audio_cb(void *opaque, int free_bytes)
{
    (void)opaque;
    (void)free_bytes;
}

/*
 * The codec's own fs-paced pull (see the drain_timer comment in the header).  We
 * fire every SAI_DRAIN_TICK_NS of virtual time and drain EXACTLY the number of
 * words the bit clock would have clocked out since the last tick (fs * elapsed,
 * with the sub-word remainder carried in drain_acc).  Pacing the FIFO to fs -- not
 * to "as fast as the backend accepts" -- is what keeps the guest at real fs: fast
 * enough that the 100 Hz callback no longer throttles it, but never faster than
 * the codec, so the wav stays byte-exact.  (Underrun->FEF is not modelled here: a
 * paced drain can't cleanly tell genuine starvation from a clean end-of-stream,
 * and the old callback's empty-check FEF was spurious; overrun->FEF, a write to a
 * full FIFO, is unaffected and still fires in the TDR write path.)
 */
#define SAI_DRAIN_TICK_NS 200000   /* 200 us; fs*200us ~= 9.6 words at 48 kHz */

static void imxrt1180_sai_drain_tick(void *opaque)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t fs = s->voice_hz;
    uint64_t want;

    if (!(s->regs[SAI_TCSR >> 2] & CSR_TE) || !s->voice || fs == 0) {
        return;                 /* disarmed: tx_update re-arms when TX resumes */
    }

    /* Words the codec would have consumed since the last tick, fs-paced. */
    s->drain_acc += (uint64_t)(now - s->drain_last_ns) * fs;
    want = s->drain_acc / NANOSECONDS_PER_SECOND;
    s->drain_acc -= want * NANOSECONDS_PER_SECOND;
    s->drain_last_ns = now;

    if (want) {
        if (imxrt1180_sai_drain(s, (int)(want * sizeof(int16_t))) > 0) {
            s->drain_out_ns = now;   /* a word just clocked toward the sink */
        }
        imxrt1180_sai_update_irq(s);
    }
    timer_mod(s->drain_timer, now + SAI_DRAIN_TICK_NS);
}

/*
 * CODEC-PIPELINE GRACE.  When the FIFO SRAM has just emptied, the last words have
 * left the FIFO but the host audio backend still holds them in its own output
 * buffer, which it flushes to the wav on its ~100 Hz timer.  A guest that polls
 * TFR for "FIFO empty" and then exits via semihosting would terminate QEMU before
 * that flush ran -- losing up to one backend chunk off the tail of the file (the
 * FIFO-empty test firmware waits for exactly this, and it was byte-exact until the
 * fs-paced drain made the FIFO empty faster than the backend writes).
 *
 * On silicon the serializer clocks the tail out in well under a sample period and
 * "FIFO empty" genuinely means "the codec got everything"; the backend's chunked
 * output is a host artifact.  So for a short window after the last word is clocked
 * we report the FIFO as NOT-yet-empty, which keeps the guest's drain-wait spinning
 * (advancing wall time) just long enough for the backend to write the tail.
 */
#define SAI_DRAIN_GRACE_NS 50000000   /* 50 ms: several backend flush ticks */

static bool imxrt1180_sai_tx_draining(IMXRT1180SAIState *s)
{
    if (!(s->regs[SAI_TCSR >> 2] & CSR_TE) || !s->voice || s->voice_hz == 0 ||
        s->tx_count != 0 || s->drain_out_ns == 0) {
        return false;
    }
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->drain_out_ns
           < SAI_DRAIN_GRACE_NS;
}

/* (Re)open the audio voice whenever the rate the guest programmed changes. */
static void imxrt1180_sai_tx_update(IMXRT1180SAIState *s)
{
    uint32_t tcsr = s->regs[SAI_TCSR >> 2];
    bool enabled = (tcsr & CSR_TE) != 0;
    uint32_t nchan = 0, word_bits = 0;
    uint32_t hz = enabled ? imxrt1180_sai_tx_hz(s, &nchan, &word_bits) : 0;

    if (!s->audio_be) {
        return;
    }

    if (enabled && hz && word_bits != 16) {
        /*
         * We only render 16-bit words.  DECLINE, VISIBLY -- do not silently emit
         * garbage at a width we did not implement.  (The guest still gets its
         * FIFO semantics; it just gets no audio, and the operator is told why.)
         */
        qemu_log_mask(LOG_UNIMP, "imxrt1180-sai: TX word width %u not modelled "
                      "(only 16-bit); no audio will be rendered\n", word_bits);
        hz = 0;
    }

    if (hz != s->voice_hz) {
        if (hz) {
            struct audsettings as = {
                .freq = hz,
                .nchannels = (nchan >= 2u) ? 2 : 1,
                .fmt = AUDIO_FORMAT_S16,
                .big_endian = false,
            };
            s->voice = audio_be_open_out(s->audio_be, s->voice, "imxrt1180-sai.tx",
                                         s, imxrt1180_sai_audio_cb, &as);
        }
        s->voice_hz = hz;
    }

    if (s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice, enabled && hz);
    }

    /*
     * Arm the fs-paced drain while TX is actively rendering; disarm otherwise so
     * an idle SAI costs nothing.  Only (re)seed the pacing clock when arming from
     * idle -- if the timer is already pending, leave drain_last_ns/drain_acc alone
     * so an unrelated register poke does not reset the fs accounting mid-stream.
     */
    if (enabled && hz && s->voice) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (!timer_pending(s->drain_timer)) {
            s->drain_last_ns = now;
            s->drain_acc = 0;
        }
        timer_mod(s->drain_timer, now + SAI_DRAIN_TICK_NS);
    } else {
        timer_del(s->drain_timer);
    }
}

static void imxrt1180_sai_update_irq(IMXRT1180SAIState *s)
{
    uint32_t tcsr = s->regs[SAI_TCSR >> 2];
    uint32_t rcsr = s->regs[SAI_RCSR >> 2];
    uint32_t watermark = TCR1_TFW(s->regs[SAI_TCR1 >> 2]);
    uint32_t tflags = 0;
    bool dma_tx = false;

    if (tcsr & CSR_EN) {
        /*
         * ⭐ CORRECTED 2026-07-17: FRF and FWF were SWAPPED-AND-WRONG.  The old code
         * had FRF = "has room at all" (tx_count < depth) and FWF = "<= watermark".
         * Neither matches the silicon, and FWF held what is actually FRF's meaning.
         *
         * Authoritative meaning, from the MIMXRT1189 SDK (fsl_sai.h enum comments:
         * FRIE "means reached watermark", FWIE "means the FIFO is empty") and cross-
         * checked against the sai_edma driver, whose per-request burst is exactly
         * FIFO_depth - watermark -- i.e. the request fires at the watermark and the
         * DMA refills back to full:
         *
         *   FRF (Request) -- the FIFO has drained TO/below the watermark: the normal
         *                    refill trigger.  This is what the DMA (FRDE) keys on and
         *                    what the SDK's polling player (SAI_WriteBlocking waits on
         *                    FWF; the driver_example polls FRF) uses for a slot.
         *   FWF (Warning) -- the FIFO is EMPTY: underrun is imminent.
         *
         * A DMA line keyed on the OLD FRF would have fired with the FIFO one slot
         * short of full and then written FIFO_depth-watermark words -- an overrun.
         * See the retraction note in PERIPHERALS.md.
         */
        if (s->tx_count <= watermark) {
            tflags |= CSR_FRF;
        }
        if (s->tx_count == 0) {
            tflags |= CSR_FWF;
        }

        /*
         * TX DMA request line.  Asserted while a DMA-enable bit is set AND its paired
         * FIFO condition holds; the eDMA services one minor loop per assertion and
         * the line falls of its own accord as those TDR writes push tx_count past the
         * threshold -- the same "a minor loop lowers its own request" handshake the
         * eDMA relies on for RX (see imxrt1180_edma.c).  The SDK sai_edma driver uses
         * FRDE; FWDE is modelled too because the register exposes both.
         */
        dma_tx = ((tcsr & CSR_FRDE) && (s->tx_count <= watermark)) ||
                 ((tcsr & CSR_FWDE) && (s->tx_count == 0));
    }
    tflags |= tcsr & CSR_STICKY_FLAGS;

    uint32_t rflags = rcsr & CSR_STICKY_FLAGS;
    bool tx = (((tflags >> 16) & 0x1Fu) & ((tcsr >> CSR_IE_TO_FLAG_SHIFT) & 0x1Fu)) != 0;
    bool rx = (((rflags >> 16) & 0x1Fu) & ((rcsr >> CSR_IE_TO_FLAG_SHIFT) & 0x1Fu)) != 0;

    qemu_set_irq(s->irq, tx || rx);
    qemu_set_irq(s->dma_tx_req, dma_tx);
}

static uint64_t imxrt1180_sai_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(opaque);
    uint32_t v = (off < IMXRT1180_SAI_SIZE) ? s->regs[off >> 2] : 0;
    uint32_t watermark = TCR1_TFW(s->regs[SAI_TCR1 >> 2]);

    switch (off) {
    case SAI_VERID:
        return SAI_VERID_VALUE;
    case SAI_PARAM:
        return s->param;        /* per-instance, from the RM */
    case SAI_TCSR:
        /* Soft-reset bits are momentary: they never read back as set. */
        v &= ~(CSR_SR | CSR_FR);
        v &= ~(CSR_FRF | CSR_FWF);
        if (v & CSR_TE) {
            /* FRF = drained to watermark; FWF = empty. See update_irq. */
            if (s->tx_count <= watermark) {
                v |= CSR_FRF;
            }
            /* FWF = FIFO empty -- but hold it back during the codec-pipeline grace
             * so a driver polling FWF for "drain done" waits for the real flush. */
            if (s->tx_count == 0 && !imxrt1180_sai_tx_draining(s)) {
                v |= CSR_FWF;
            }
        }
        return v;
    case SAI_RCSR:
        v &= ~(CSR_SR | CSR_FR);
        v &= ~(CSR_FRF | CSR_FWF | CSR_FEF);
        return v;
    case SAI_TFR0:
    case SAI_TFR0 + 4: {
        /* REAL pointers now. They used to both read 0 -- "FIFO always empty" --
         * which told a driver it could push forever. */
        uint32_t rfp = s->tx_rptr & 0x3Fu;
        /* During the codec-pipeline grace the SRAM is empty (rptr == wptr) but the
         * tail is still being flushed downstream; report rptr != wptr so a driver
         * waiting on "rptr == wptr" keeps spinning until the flush completes. */
        if (imxrt1180_sai_tx_draining(s)) {
            rfp = (s->tx_wptr + IMXRT1180_SAI_FIFO_MAX - 1u) & 0x3Fu;
        }
        return (rfp << TFR_RFP_SHIFT) | ((s->tx_wptr & 0x3Fu) << TFR_WFP_SHIFT);
    }
    case SAI_RFR0:
    case SAI_RFR0 + 4:
        return 0;   /* RX FIFO empty (RX path not modelled) */
    case SAI_RDR0:
    case SAI_RDR0 + 4:
        return 0;
    default:
        return v;
    }
}

static void imxrt1180_sai_write(void *opaque, hwaddr off, uint64_t value,
                                unsigned size)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(opaque);
    uint32_t val = value;

    if (off >= IMXRT1180_SAI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case SAI_VERID:
    case SAI_PARAM:
        return;   /* read-only */
    case SAI_TFR0:
    case SAI_TFR0 + 4:
    case SAI_RFR0:
    case SAI_RFR0 + 4:
    case SAI_RDR0:
    case SAI_RDR0 + 4:
        return;   /* read-only FIFO/data registers */
    case SAI_TCSR: {
        uint32_t cur = s->regs[off >> 2];

        cur &= ~(val & CSR_FLAGS_W1C);
        cur = (cur & CSR_FLAGS_W1C) | (val & ~CSR_FLAGS_W1C);
        if (val & CSR_FR) {          /* FIFO reset: really empty it */
            s->tx_count = s->tx_rptr = s->tx_wptr = 0;
        }
        cur &= ~(CSR_SR | CSR_FR);   /* momentary */
        s->regs[off >> 2] = cur;
        imxrt1180_sai_tx_update(s);
        imxrt1180_sai_update_irq(s);
        return;
    }
    case SAI_RCSR: {
        uint32_t cur = s->regs[off >> 2];

        cur &= ~(val & CSR_FLAGS_W1C);
        cur = (cur & CSR_FLAGS_W1C) | (val & ~CSR_FLAGS_W1C);
        cur &= ~(CSR_SR | CSR_FR);
        s->regs[off >> 2] = cur;
        imxrt1180_sai_update_irq(s);
        return;
    }
    case SAI_TDR0:
    case SAI_TDR0 + 4:
        /*
         * THE SAMPLE. It used to be discarded here.
         *
         * A write to a FULL FIFO is an OVERRUN, and FEF is the flag the guest reads
         * to find out.  Dropping the word silently -- which is what "accepted and
         * discarded" did for every word -- is the thing this whole file exists to
         * stop doing.
         */
        if (s->tx_count >= s->fifo_depth) {
            s->regs[SAI_TCSR >> 2] |= CSR_FEF;
            imxrt1180_sai_update_irq(s);
            return;
        }
        s->tx_fifo[s->tx_wptr] = val;
        s->tx_wptr = (s->tx_wptr + 1u) % IMXRT1180_SAI_FIFO_MAX;
        s->tx_count++;
        imxrt1180_sai_update_irq(s);
        return;
    case SAI_TCR1:
    case SAI_TCR2:
    case SAI_TCR4:
    case SAI_TCR5:
        s->regs[off >> 2] = val;
        imxrt1180_sai_tx_update(s);   /* the rate may have just changed */
        imxrt1180_sai_update_irq(s);
        return;
    default:
        s->regs[off >> 2] = val;
        return;
    }
}

static const MemoryRegionOps imxrt1180_sai_ops = {
    .read = imxrt1180_sai_read,
    .write = imxrt1180_sai_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void imxrt1180_sai_reset(DeviceState *dev)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->tx_count = s->tx_rptr = s->tx_wptr = 0;
    s->voice_hz = 0;
    s->drain_last_ns = 0;
    s->drain_acc = 0;
    s->drain_out_ns = 0;
    qemu_set_irq(s->dma_tx_req, false);
    if (s->drain_timer) {
        timer_del(s->drain_timer);
    }
    if (s->audio_be && s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
}

static void imxrt1180_sai_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(dev);

    /*
     * PARAM DESCRIBES THE SILICON.  The FIFO we HOLD may be larger (one array
     * serves all four instances), but it must never be SMALLER than what we told
     * the guest -- a driver sizes its bursts against PARAM.
     *
     *   ⭐ THE INVARIANT IS model >= advertised, NOT model == advertised.
     */
    s->fifo_depth = 1u << PARAM_FIFO(s->param);
    if (s->fifo_depth > IMXRT1180_SAI_FIFO_MAX) {
        error_setg(errp, "imxrt1180-sai: PARAM 0x%08x advertises a %u-word FIFO; "
                   "this model holds %u", s->param, s->fifo_depth,
                   IMXRT1180_SAI_FIFO_MAX);
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_sai_ops, s,
                          TYPE_IMXRT1180_SAI, IMXRT1180_SAI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_out_named(dev, &s->dma_tx_req, "dma-tx-req", 1);

    /*
     * No audiodev is not an error -- it is the common case, and it must not be a
     * SILENT one.  audio_be_check() leaves audio_be NULL; every audio path above
     * tests it, and the block still behaves correctly as a FIFO.
     */
    if (!audio_be_check(&s->audio_be, NULL)) {
        s->audio_be = NULL;
    }

    /* The codec's fs-paced FIFO pull; armed by tx_update while TX is active. */
    s->drain_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  imxrt1180_sai_drain_tick, s);
}

static const Property imxrt1180_sai_props[] = {
    /* The RM's per-instance PARAM. The SoC supplies it; there is no default,
     * because there is no such thing as "the" SAI on this chip. */
    DEFINE_PROP_UINT32("param", IMXRT1180SAIState, param, 0x00050402u),
    DEFINE_PROP_UINT32("clk-root", IMXRT1180SAIState, clk_root, 0),
    DEFINE_PROP_LINK("ccm", IMXRT1180SAIState, ccm, TYPE_IMXRT1180_CCM, void *),
};

static const VMStateDescription vmstate_imxrt1180_sai = {
    .name = TYPE_IMXRT1180_SAI,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180SAIState, IMXRT1180_SAI_SIZE / 4),
        VMSTATE_UINT32_ARRAY(tx_fifo, IMXRT1180SAIState, IMXRT1180_SAI_FIFO_MAX),
        VMSTATE_UINT32(tx_count, IMXRT1180SAIState),
        VMSTATE_UINT32(tx_rptr, IMXRT1180SAIState),
        VMSTATE_UINT32(tx_wptr, IMXRT1180SAIState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_sai_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_sai_realize;
    device_class_set_legacy_reset(dc, imxrt1180_sai_reset);
    dc->vmsd = &vmstate_imxrt1180_sai;
    device_class_set_props(dc, imxrt1180_sai_props);
}

static const TypeInfo imxrt1180_sai_types[] = {
    {
        .name          = TYPE_IMXRT1180_SAI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180SAIState),
        .class_init    = imxrt1180_sai_class_init,
    },
};

DEFINE_TYPES(imxrt1180_sai_types)
