/*
 * NXP i.MX RT1180 ASRC (Asynchronous Sample Rate Converter) — data-path model.
 * See the header for the rationale (real resampling, linear-interp not polyphase).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/audio/imxrt1180_asrc.h"
#include "hw/misc/imxrt1180_sai.h"
#include "migration/vmstate.h"
#include <math.h>

/* --- register offsets ----------------------------------------------------- */
#define ASRCTR   0x00   /* control (enable, ATSA task-start bits)          */
#define ASRIER   0x04   /* interrupt enable                                */
#define ASRCNCR  0x0C   /* channel-number config (ANCA = pair A channels)  */
#define ASRCFG   0x10   /* filter config + INIRQA init-done status         */
#define ASRCSR   0x14   /* clock source select                             */
#define ASRCDR1  0x18   /* clock dividers: in/out divider+prescaler A,B    */
#define ASRCDR2  0x1C   /* clock dividers: C                               */
#define ASRSTR   0x20   /* status: AIDEA (in room) / AODFA (out ready)     */
#define ASRDIA   0x60   /* pair-A input data  (write)                      */
#define ASRDOA   0x64   /* pair-A output data (read)                       */
/* B/C data regs at 0x68/0x6C (B) and 0x70/0x74 (C). */
#define ASRMCRA  0xA0   /* pair-A misc: in/out FIFO thresholds             */
#define ASRFSTA  0xA4   /* pair-A FIFO status (fill levels)                */

/* --- field masks/shifts --------------------------------------------------- */
#define ASRCTR_ASRCEN       (1u << 0)        /* module enable                */
#define ASRCTR_ATSA_SHIFT   20               /* task-start A (init the pair) */
#define ASRCNCR_ANCA_MASK   0xF
#define ASRCFG_INIRQA       (1u << 21)       /* pair-A init done             */
#define ASRSTR_AIDEA        (1u << 0)        /* +pair: input FIFO has room   */
#define ASRSTR_AODFA        (1u << 3)        /* +pair: output FIFO ready     */
#define ASRMCR_INFIFO_MASK  0x3Fu
#define ASRMCR_OUTFIFO_SHIFT 12
#define ASRMCR_OUTFIFO_MASK 0x3Fu
/* ASRCDR1 pair-A: AICPA[2:0] in-presc, AICDA[5:3] in-div, AOCPA[14:12] out-presc,
 * AOCDA[17:15] out-div. sampleRate = srcClk / ((div+1) * 2^presc). */

static inline uint32_t fifo_fill(uint32_t head, uint32_t tail)
{
    return (tail - head) & (IMXRT1180_ASRC_FIFO - 1);
}

/*
 * Resolve an ASRCSR clock-source select (kASRC_ClockSource* value) to its actual
 * frequency in Hz, or 0 if this model cannot (RX bit clocks, SPDIF, the SAIx clock
 * ROOTs, MIC/MQS -- none of whose live rate we model).  Only the SAIn TX bit clocks
 * (selects 0/2/4/6) are resolvable, via the linked SAI devices.  0 is honest: the
 * caller must fall back, not fabricate a frequency.
 */
static uint32_t asrc_src_hz(IMXRT1180ASRCState *s, uint32_t sel)
{
    /* kASRC_ClockSourceBitClock{0,2,4,6}_SAI{1,2,3,4}_TX = 0,2,4,6. */
    if (sel <= 6 && (sel & 1u) == 0) {
        DeviceState *sai = s->sai[sel / 2];
        return sai ? imxrt1180_sai_tx_bclk_hz(sai) : 0;
    }
    return 0;   /* RX / SPDIF / clock-root / MIC / MQS: not modelled */
}

/*
 * Decode pair A's in:out sample-rate ratio (output frames per input frame).
 *
 *   in_rate  = inSrcHz  / (in_div  * 2^in_presc)
 *   out_rate = outSrcHz / (out_div * 2^out_presc)
 *   ratio    = out_rate/in_rate = (outSrcHz/inSrcHz) * in_period/out_period
 *
 * The ASRCDR dividers give in_period/out_period.  The (outSrcHz/inSrcHz) factor is
 * 1 when input and output share ONE clock source -- exactly true for every m2m
 * example (ASRCSR AICSA == AOCSA), so the sources cancel with no need to know their
 * frequency.  For a TRUE-ASYNC conversion (different sources) the factor is real
 * and this resolves it from the two SAI TX bit clocks.  If a differing source is
 * one we cannot resolve, we flag it and fall back to the divider ratio rather than
 * invent a frequency.
 */
static double pair_ratio_out_per_in(IMXRT1180ASRCState *s)
{
    uint32_t dr = s->regs[ASRCDR1 / 4];
    uint32_t in_presc  =  dr        & 0x7;
    uint32_t in_div    = ((dr >> 3) & 0x7) + 1;
    uint32_t out_presc = (dr >> 12) & 0x7;
    uint32_t out_div   = ((dr >> 15) & 0x7) + 1;
    double in_period  = (double)in_div  * (double)(1u << in_presc);
    double out_period = (double)out_div * (double)(1u << out_presc);
    double src_factor = 1.0;
    uint32_t sr = s->regs[ASRCSR / 4];
    uint32_t in_sel  =  sr        & 0xF;   /* ASRCSR.AICSA */
    uint32_t out_sel = (sr >> 12) & 0xF;   /* ASRCSR.AOCSA */

    if (out_period <= 0) {
        return 1.0;
    }
    if (in_sel != out_sel) {
        uint32_t in_hz = asrc_src_hz(s, in_sel), out_hz = asrc_src_hz(s, out_sel);
        if (in_hz && out_hz) {
            src_factor = (double)out_hz / (double)in_hz;   /* true-async factor */
        } else {
            qemu_log_mask(LOG_UNIMP, "imxrt1180-asrc: async clock sources in=%u "
                          "out=%u -- only SAIn TX bit clocks are resolvable, so the "
                          "in:out frequency factor is not modelled here; using the "
                          "ASRCDR divider ratio alone (in_hz=%u out_hz=%u)\n",
                          in_sel, out_sel, in_hz, out_hz);
        }
    }
    return src_factor * in_period / out_period;
}

/*
 * FIR resampler kernel.  HALF taps each side (16-tap window); the input history
 * ring must hold at least 2*HALF frames.
 */
#define ASRC_FIR_HALF  8

/*
 * A windowed-sinc lowpass interpolation kernel evaluated at fractional offset `x`
 * (in input samples) with cutoff `fc` (cycles per input sample), Hann-windowed over
 * [-HALF, HALF].
 *
 *   ⭐ THIS IS NOT NXP's POLYPHASE FILTER.  The silicon ASRC runs a specific
 *   multi-tap polyphase FIR whose coefficients are not in the RM or the SDK, so the
 *   output SAMPLE VALUES cannot bit-match hardware and this file does not claim to.
 *   What this DOES model faithfully is the CLASS of algorithm -- a real
 *   anti-imaging / anti-aliasing bandlimited resampler, not the crude linear
 *   interpolation it replaces: the cutoff tracks the ratio so a DOWN-conversion
 *   rejects content above the output Nyquist instead of aliasing it into the band
 *   (the whole reason the hardware uses a FIR).  Tested against DSP first
 *   principles (unity DC gain, stopband rejection), never against silicon values.
 */
static double asrc_winsinc(double x, double fc)
{
    double a, s, w;

    if (fabs(x) >= (double)ASRC_FIR_HALF) {
        return 0.0;
    }
    w = 0.5 * (1.0 + cos(M_PI * x / (double)ASRC_FIR_HALF));   /* Hann window */
    a = 2.0 * fc * x;
    s = (a == 0.0) ? 1.0 : sin(M_PI * a) / (M_PI * a);         /* sinc(2*fc*x) */
    return 2.0 * fc * s * w;
}

/*
 * Run the streaming polyphase windowed-sinc resampler for pair `p` after input
 * frames have been pushed into in_fifo.  Each output frame at input-position
 * `out_pos` is the normalized windowed-sinc convolution of the surrounding input
 * frames; normalizing by the tap-sum makes the DC gain EXACTLY 1 (a constant in is
 * a constant out) regardless of the window.  The cutoff `fc` is the output Nyquist
 * when down-converting (anti-aliasing) and the input Nyquist when up-converting
 * (anti-imaging).  Output lags input by ~HALF frames (the filter's group delay), so
 * the first HALF outputs are an edge transient and callers skip them.
 */
static void asrc_resample(IMXRT1180ASRCState *s, unsigned p)
{
    IMXRT1180ASRCPair *pr = &s->pair[p];
    uint32_t ch = pr->channels ? pr->channels : 2;
    double ratio = pair_ratio_out_per_in(s);   /* output frames per input frame */
    double step, fc;

    if (ratio <= 0) {
        ratio = 1.0;
    }
    step = 1.0 / ratio;
    fc = (ratio < 1.0) ? 0.5 * ratio : 0.5;    /* min(in, out) Nyquist */

    while (fifo_fill(pr->in_head, pr->in_tail) >= ch) {
        /* consume one input frame into the per-channel history ring */
        uint32_t slot = (uint32_t)(pr->in_count & (IMXRT1180_ASRC_HIST - 1));
        for (uint32_t c = 0; c < ch && c < 2; c++) {
            pr->hist[c][slot] = pr->in_fifo[pr->in_head];
            pr->in_head = (pr->in_head + 1) & (IMXRT1180_ASRC_FIFO - 1);
        }
        pr->in_count++;
        if (!pr->started) {
            pr->out_pos = 0.0;
            pr->started = true;
        }

        /* emit every output whose right-hand context (HALF frames past out_pos) is
         * now available; the left side is zero-padded at the very start. */
        while ((double)pr->in_count - 1.0 >= pr->out_pos + (double)ASRC_FIR_HALF) {
            int64_t i0 = (int64_t)floor(pr->out_pos);
            double frac = pr->out_pos - (double)i0;

            for (uint32_t c = 0; c < ch && c < 2; c++) {
                double sum = 0.0, wsum = 0.0;
                for (int k = -(ASRC_FIR_HALF - 1); k <= ASRC_FIR_HALF; k++) {
                    double g = asrc_winsinc(frac - (double)k, fc);
                    int64_t f = i0 + k;
                    int32_t sample = (f >= 0 && (uint64_t)f < pr->in_count)
                        ? pr->hist[c][(uint32_t)((uint64_t)f & (IMXRT1180_ASRC_HIST - 1))]
                        : 0;
                    sum  += (double)sample * g;
                    wsum += g;
                }
                if (fifo_fill(pr->out_head, pr->out_tail) < IMXRT1180_ASRC_FIFO - 1) {
                    int32_t o = (wsum != 0.0) ? (int32_t)lrint(sum / wsum) : 0;
                    pr->out_fifo[pr->out_tail] = o;
                    pr->out_tail = (pr->out_tail + 1) & (IMXRT1180_ASRC_FIFO - 1);
                }
            }
            pr->out_pos += step;
        }
    }
}

static void asrc_update_irq(IMXRT1180ASRCState *s)
{
    /* IRQ when an enabled interrupt condition holds (output ready). Keep simple:
     * assert if any pair has output ready AND its interrupt is enabled (ASRIER). */
    bool active = false;
    for (unsigned p = 0; p < IMXRT1180_ASRC_PAIRS; p++) {
        if (fifo_fill(s->pair[p].out_head, s->pair[p].out_tail) > 0 &&
            (s->regs[ASRIER / 4] & (ASRSTR_AODFA << p))) {
            active = true;
        }
    }
    qemu_set_irq(s->irq, active);
}

static uint64_t asrc_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180ASRCState *s = opaque;

    switch (off) {
    case ASRSTR: {
        uint32_t v = 0;
        /*
         * AIDEA (input has room) / AODFA (output ready) describe a RUNNING
         * converter's FIFOs.  While the module is disabled they are meaningless,
         * and the RM's ASRSTR reset value is 0x0 -- but an empty input FIFO reads
         * as "has room", so reporting them unconditionally makes a freshly-reset
         * ASRC (ASRCEN=0) hand the guest 0x7, a value the silicon never produces.
         * Gate on ASRCEN: the fsl_asrc driver enables the module before it ever
         * polls ASRSTR, so this is invisible to it and correct at reset.
         */
        if (!(s->regs[ASRCTR / 4] & ASRCTR_ASRCEN)) {
            return 0;
        }
        for (unsigned p = 0; p < IMXRT1180_ASRC_PAIRS; p++) {
            IMXRT1180ASRCPair *pr = &s->pair[p];
            uint32_t owm = (s->regs[ASRMCRA / 4] >> ASRMCR_OUTFIFO_SHIFT)
                           & ASRMCR_OUTFIFO_MASK;
            /* input FIFO has room (we drain it inline, so almost always) */
            if (fifo_fill(pr->in_head, pr->in_tail) < IMXRT1180_ASRC_FIFO / 2) {
                v |= (ASRSTR_AIDEA << p);
            }
            /* output ready when it holds at least the watermark */
            if (fifo_fill(pr->out_head, pr->out_tail) > (owm ? owm : 1)) {
                v |= (ASRSTR_AODFA << p);
            }
        }
        return v;
    }
    case ASRDOA: case 0x6C: case 0x74: {       /* pair A / B / C output pop */
        unsigned p = (off == ASRDOA) ? 0 : (off == 0x6C) ? 1 : 2;
        IMXRT1180ASRCPair *pr = &s->pair[p];
        int32_t o = 0;
        if (fifo_fill(pr->out_head, pr->out_tail) > 0) {
            o = pr->out_fifo[pr->out_head];
            pr->out_head = (pr->out_head + 1) & (IMXRT1180_ASRC_FIFO - 1);
        }
        asrc_update_irq(s);
        return (uint32_t)o & 0xFFFFFF;
    }
    case ASRCFG:
        /* Init completes as soon as the module is enabled: report INIRQ{A,B,C}
         * (bits 21-23) so ASRC_SetChannelPairConfig's init-done poll retires. */
        return s->regs[ASRCFG / 4] |
               ((s->regs[ASRCTR / 4] & ASRCTR_ASRCEN) ? (0x7u << 21) : 0);

    case ASRFSTA: {                            /* pair A FIFO fill levels */
        uint32_t inf = fifo_fill(s->pair[0].in_head, s->pair[0].in_tail);
        uint32_t outf = fifo_fill(s->pair[0].out_head, s->pair[0].out_tail);
        return (inf & 0x7F) | ((outf & 0x7F) << 12);
    }
    default:
        return off < IMXRT1180_ASRC_SIZE ? s->regs[off / 4] : 0;
    }
}

static void asrc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IMXRT1180ASRCState *s = opaque;
    uint32_t v = val;

    switch (off) {
    case ASRDIA: case 0x68: case 0x70: {       /* pair A / B / C input push */
        unsigned p = (off == ASRDIA) ? 0 : (off == 0x68) ? 1 : 2;
        IMXRT1180ASRCPair *pr = &s->pair[p];
        int32_t sample = (int32_t)(int16_t)(v & 0xFFFF);   /* 16-bit signed */
        if (fifo_fill(pr->in_head, pr->in_tail) < IMXRT1180_ASRC_FIFO - 1) {
            pr->in_fifo[pr->in_tail] = sample;
            pr->in_tail = (pr->in_tail + 1) & (IMXRT1180_ASRC_FIFO - 1);
        }
        asrc_resample(s, p);
        asrc_update_irq(s);
        return;
    }
    case ASRCNCR:
        s->regs[off / 4] = v;
        s->pair[0].channels = v & ASRCNCR_ANCA_MASK;
        s->pair[1].channels = (v >> 4) & 0xF;
        s->pair[2].channels = (v >> 8) & 0xF;
        return;
    case ASRCTR:
        /* A task-start (ATSA) for a pair (re)initialises its resampler. */
        for (unsigned p = 0; p < IMXRT1180_ASRC_PAIRS; p++) {
            if (v & (1u << (ASRCTR_ATSA_SHIFT + p))) {
                IMXRT1180ASRCPair *pr = &s->pair[p];
                pr->in_head = pr->in_tail = pr->out_head = pr->out_tail = 0;
                pr->in_count = 0;
                pr->out_pos = 0.0;
                pr->started = false;
                memset(pr->hist, 0, sizeof(pr->hist));
            }
        }
        s->regs[off / 4] = v;
        return;
    default:
        if (off < IMXRT1180_ASRC_SIZE) {
            s->regs[off / 4] = v;
        }
        return;
    }
}

static const MemoryRegionOps asrc_ops = {
    .read = asrc_read,
    .write = asrc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void asrc_reset(DeviceState *dev)
{
    IMXRT1180ASRCState *s = IMXRT1180_ASRC(dev);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->pair, 0, sizeof(s->pair));
}

static void asrc_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180ASRCState *s = IMXRT1180_ASRC(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &asrc_ops, s,
                          TYPE_IMXRT1180_ASRC, IMXRT1180_ASRC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_asrc = {
    .name = TYPE_IMXRT1180_ASRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180ASRCState, IMXRT1180_ASRC_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

/* SAIn TX bit clocks are selectable ASRC clock sources (ASRCSR); the SoC wires
 * these so a true-async conversion can resolve both sources' frequencies. */
static const Property asrc_props[] = {
    DEFINE_PROP_LINK("sai1", IMXRT1180ASRCState, sai[0], TYPE_IMXRT1180_SAI,
                     DeviceState *),
    DEFINE_PROP_LINK("sai2", IMXRT1180ASRCState, sai[1], TYPE_IMXRT1180_SAI,
                     DeviceState *),
    DEFINE_PROP_LINK("sai3", IMXRT1180ASRCState, sai[2], TYPE_IMXRT1180_SAI,
                     DeviceState *),
    DEFINE_PROP_LINK("sai4", IMXRT1180ASRCState, sai[3], TYPE_IMXRT1180_SAI,
                     DeviceState *),
};

static void asrc_class_init(ObjectClass *k, const void *d)
{
    DeviceClass *dc = DEVICE_CLASS(k);
    dc->realize = asrc_realize;
    device_class_set_legacy_reset(dc, asrc_reset);
    dc->vmsd = &vmstate_asrc;
    device_class_set_props(dc, asrc_props);
}

static const TypeInfo asrc_types[] = {{
    .name = TYPE_IMXRT1180_ASRC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180ASRCState),
    .class_init = asrc_class_init,
}};
DEFINE_TYPES(asrc_types)
