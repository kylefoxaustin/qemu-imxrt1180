/*
 * NXP i.MX RT1180 ASRC (Asynchronous Sample Rate Converter) — data-path model.
 * See the header for the rationale (real resampling, linear-interp not polyphase).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/audio/imxrt1180_asrc.h"
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

/* Decode pair A's in:out sample-rate ratio (out per in) from ASRCDR1.  The
 * source clock cancels when in and out share it (the m2m example) -- exact
 * there, a first approximation for true-async sources (flagged). */
static double pair_ratio_out_per_in(IMXRT1180ASRCState *s)
{
    uint32_t dr = s->regs[ASRCDR1 / 4];
    uint32_t in_presc  =  dr        & 0x7;
    uint32_t in_div    = ((dr >> 3) & 0x7) + 1;
    uint32_t out_presc = (dr >> 12) & 0x7;
    uint32_t out_div   = ((dr >> 15) & 0x7) + 1;
    double in_period  = (double)in_div  * (double)(1u << in_presc);
    double out_period = (double)out_div * (double)(1u << out_presc);
    if (out_period <= 0) {
        return 1.0;
    }
    return in_period / out_period;         /* out_rate/in_rate = in_per/out_per */
}

/* Run the streaming linear-interpolation resampler for pair `p` after an input
 * frame (all channels of one sample instant) has been pushed. */
static void asrc_resample(IMXRT1180ASRCState *s, unsigned p)
{
    IMXRT1180ASRCPair *pr = &s->pair[p];
    uint32_t ch = pr->channels ? pr->channels : 2;
    double ratio = pair_ratio_out_per_in(s);   /* output frames per input frame */
    if (ratio <= 0) {
        ratio = 1.0;
    }
    double step = 1.0 / ratio;                  /* input advance per output frame */

    /* need a whole input frame (ch samples) buffered to form `curr` */
    while (fifo_fill(pr->in_head, pr->in_tail) >= ch) {
        int32_t curr[2] = {0, 0};
        for (uint32_t c = 0; c < ch && c < 2; c++) {
            curr[c] = pr->in_fifo[pr->in_head];
            pr->in_head = (pr->in_head + 1) & (IMXRT1180_ASRC_FIFO - 1);
        }
        if (!pr->have_prev) {
            pr->prev[0] = curr[0];
            pr->prev[1] = curr[1];
            pr->have_prev = true;
            continue;
        }
        /* emit output frames at fractional positions in [phase, 1) between the
         * previous and current input frame (phase carries across frames). */
        double phase = (double)pr->phase / 4294967296.0;   /* Q32 -> [0,1) */
        while (phase < 1.0) {
            for (uint32_t c = 0; c < ch && c < 2; c++) {
                double v = (double)pr->prev[c] * (1.0 - phase)
                         + (double)curr[c] * phase;
                int32_t o = (int32_t)lrint(v);
                if (fifo_fill(pr->out_head, pr->out_tail) < IMXRT1180_ASRC_FIFO - 1) {
                    pr->out_fifo[pr->out_tail] = o;
                    pr->out_tail = (pr->out_tail + 1) & (IMXRT1180_ASRC_FIFO - 1);
                }
            }
            phase += step;
        }
        pr->phase = (uint64_t)((phase - 1.0) * 4294967296.0);
        pr->prev[0] = curr[0];
        pr->prev[1] = curr[1];
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
                pr->phase = 0; pr->have_prev = false;
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

static void asrc_class_init(ObjectClass *k, const void *d)
{
    DeviceClass *dc = DEVICE_CLASS(k);
    dc->realize = asrc_realize;
    device_class_set_legacy_reset(dc, asrc_reset);
    dc->vmsd = &vmstate_asrc;
}

static const TypeInfo asrc_types[] = {{
    .name = TYPE_IMXRT1180_ASRC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180ASRCState),
    .class_init = asrc_class_init,
}};
DEFINE_TYPES(asrc_types)
