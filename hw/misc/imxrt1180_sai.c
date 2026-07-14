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
#define CSR_FRF     (1u << 16)  /* FIFO request flag        */
#define CSR_FWF     (1u << 17)  /* FIFO warning flag        */
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

/* Drain the TX FIFO into the audio backend.  This is the only place samples
 * leave the model, and it is the reason the wav file has anything in it. */
static void imxrt1180_sai_audio_cb(void *opaque, int free_bytes)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(opaque);
    uint32_t tcsr = s->regs[SAI_TCSR >> 2];
    int16_t buf[IMXRT1180_SAI_FIFO_MAX];
    size_t n = 0;

    if (!(tcsr & CSR_TE)) {
        return;
    }

    while (n < ARRAY_SIZE(buf) && (int)((n + 1) * sizeof(int16_t)) <= free_bytes &&
           s->tx_count > 0) {
        /*
         * A 32-bit FIFO word carries one word of audio.  For the 16-bit case the
         * SDK writes the sample right-justified, so the low half IS the sample.
         */
        buf[n++] = (int16_t)(s->tx_fifo[s->tx_rptr] & 0xFFFFu);
        s->tx_rptr = (s->tx_rptr + 1u) % IMXRT1180_SAI_FIFO_MAX;
        s->tx_count--;
    }

    if (n) {
        audio_be_write(s->audio_be, s->voice, buf, n * sizeof(int16_t));
    } else if (free_bytes > 0) {
        /*
         * The transmitter is enabled, the codec wants a sample, and the FIFO is
         * empty.  That is an UNDERRUN, and it is exactly what the guest's FEF flag
         * exists to say.  Tell the guest -- do not paper over it with silence.
         */
        s->regs[SAI_TCSR >> 2] |= CSR_FEF;
    }
}

static void imxrt1180_sai_update_irq(IMXRT1180SAIState *s);

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
}

static void imxrt1180_sai_update_irq(IMXRT1180SAIState *s)
{
    uint32_t tcsr = s->regs[SAI_TCSR >> 2];
    uint32_t rcsr = s->regs[SAI_RCSR >> 2];
    uint32_t watermark = TCR1_TFW(s->regs[SAI_TCR1 >> 2]);
    uint32_t tflags = 0;

    if (tcsr & CSR_EN) {
        /*
         * FWF: the FIFO is at or below its watermark -- i.e. it WANTS DATA.
         * FRF: the FIFO has room at all.
         *
         * These used to be asserted unconditionally whenever TE was set, because
         * the FIFO was imaginary and "always had space".  A DMA request line keyed
         * on FRF would then be asserted forever, and a driver polling FWF would
         * never learn that it had got ahead of the codec.
         */
        if (s->tx_count < s->fifo_depth) {
            tflags |= CSR_FRF;
        }
        if (s->tx_count <= watermark) {
            tflags |= CSR_FWF;
        }
    }
    tflags |= tcsr & CSR_STICKY_FLAGS;

    uint32_t rflags = rcsr & CSR_STICKY_FLAGS;
    bool tx = (((tflags >> 16) & 0x1Fu) & ((tcsr >> CSR_IE_TO_FLAG_SHIFT) & 0x1Fu)) != 0;
    bool rx = (((rflags >> 16) & 0x1Fu) & ((rcsr >> CSR_IE_TO_FLAG_SHIFT) & 0x1Fu)) != 0;

    qemu_set_irq(s->irq, tx || rx);
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
            if (s->tx_count < s->fifo_depth) {
                v |= CSR_FRF;
            }
            if (s->tx_count <= watermark) {
                v |= CSR_FWF;
            }
        }
        return v;
    case SAI_RCSR:
        v &= ~(CSR_SR | CSR_FR);
        v &= ~(CSR_FRF | CSR_FWF | CSR_FEF);
        return v;
    case SAI_TFR0:
    case SAI_TFR0 + 4:
        /* REAL pointers now. They used to both read 0 -- "FIFO always empty" --
         * which told a driver it could push forever. */
        return ((s->tx_rptr & 0x3Fu) << TFR_RFP_SHIFT) |
               ((s->tx_wptr & 0x3Fu) << TFR_WFP_SHIFT);
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

    /*
     * No audiodev is not an error -- it is the common case, and it must not be a
     * SILENT one.  audio_be_check() leaves audio_be NULL; every audio path above
     * tests it, and the block still behaves correctly as a FIFO.
     */
    if (!audio_be_check(&s->audio_be, NULL)) {
        s->audio_be = NULL;
    }
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
