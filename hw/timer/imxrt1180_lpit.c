/*
 * NXP i.MX RT1180 LPIT — Low-Power Periodic Interrupt Timer (4 channels).
 *
 * Register-accurate against the MIMXRT1189 CMSIS PERI_LPIT.h.  Each channel is a
 * ptimer-backed 32-bit periodic down-counter (mode 0): loads TVAL, counts down
 * at the peripheral clock, sets MSR.TIFn + raises the IRQ (if MIER.TIEn) and
 * reloads.  Dual-16-bit / trigger-accumulator modes fall back to periodic.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/timer/imxrt1180_lpit.h"
#include "migration/vmstate.h"

/* Register offsets. */
#define LPIT_VERID  0x00
#define LPIT_PARAM  0x04
#define LPIT_MCR    0x08
#define LPIT_MSR    0x0C
#define LPIT_MIER   0x10
#define LPIT_SETTEN 0x14
#define LPIT_CLRTEN 0x18
#define LPIT_CH_BASE 0x20
#define LPIT_CH_STRIDE 0x10
#define LPIT_TVAL   0x0   /* within channel */
#define LPIT_CVAL   0x4
#define LPIT_TCTRL  0x8

#define MCR_M_CEN  0x1
#define MCR_SW_RST 0x2
#define TCTRL_T_EN 0x1
#define CHAN_MASK  ((1u << IMXRT1180_LPIT_NCHAN) - 1)

#define LPIT_FREQ_DEFAULT 24000000u   /* nominal LPIT bus clock */

static void lpit_update_irq(IMXRT1180LPITState *s)
{
    qemu_set_irq(s->irq, (s->msr & s->mier & CHAN_MASK) != 0);
}

/* Per-channel ptimer expiry: set the interrupt flag. */
static void lpit_channel_tick(void *opaque)
{
    IMXRT1180LPITChan *c = opaque;
    c->s->msr |= (1u << c->ch);
    lpit_update_irq(c->s);
}

static void lpit_channel_set_enabled(IMXRT1180LPITState *s, unsigned ch,
                                     bool enable)
{
    ptimer_transaction_begin(s->timer[ch]);
    if (enable && (s->mcr & MCR_M_CEN)) {
        ptimer_set_freq(s->timer[ch], s->freq);
        /* LPIT period = (TVAL + 1) clocks. */
        ptimer_set_limit(s->timer[ch], (uint64_t)s->tval[ch] + 1, 1);
        ptimer_run(s->timer[ch], 0);   /* periodic */
    } else {
        ptimer_stop(s->timer[ch]);
    }
    ptimer_transaction_commit(s->timer[ch]);
}

static uint64_t imxrt1180_lpit_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180LPITState *s = opaque;

    switch (offset) {
    case LPIT_VERID:
        return 0x01000000;
    case LPIT_PARAM:
        return IMXRT1180_LPIT_NCHAN;      /* NUM_CHANNELS */
    case LPIT_MCR:
        return s->mcr;
    case LPIT_MSR:
        return s->msr;
    case LPIT_MIER:
        return s->mier;
    case LPIT_SETTEN:
    case LPIT_CLRTEN: {
        uint32_t en = 0;
        for (unsigned ch = 0; ch < IMXRT1180_LPIT_NCHAN; ch++) {
            if (s->tctrl[ch] & TCTRL_T_EN) {
                en |= (1u << ch);
            }
        }
        return en;
    }
    default:
        if (offset >= LPIT_CH_BASE) {
            unsigned ch = (offset - LPIT_CH_BASE) / LPIT_CH_STRIDE;
            unsigned reg = (offset - LPIT_CH_BASE) % LPIT_CH_STRIDE;
            if (ch < IMXRT1180_LPIT_NCHAN) {
                switch (reg) {
                case LPIT_TVAL:
                    return s->tval[ch];
                case LPIT_CVAL:
                    return ptimer_get_count(s->timer[ch]);
                case LPIT_TCTRL:
                    return s->tctrl[ch];
                }
            }
        }
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @0x%02x\n",
                      __func__, (unsigned)offset);
        return 0;
    }
}

static void imxrt1180_lpit_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    IMXRT1180LPITState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case LPIT_MCR:
        s->mcr = v & 0xF;
        if (v & MCR_SW_RST) {
            s->msr = 0;
            for (unsigned ch = 0; ch < IMXRT1180_LPIT_NCHAN; ch++) {
                s->tval[ch] = s->tctrl[ch] = 0;
                lpit_channel_set_enabled(s, ch, false);
            }
        }
        lpit_update_irq(s);
        break;
    case LPIT_MSR:
        s->msr &= ~(v & CHAN_MASK);       /* W1C */
        lpit_update_irq(s);
        break;
    case LPIT_MIER:
        s->mier = v & CHAN_MASK;
        lpit_update_irq(s);
        break;
    case LPIT_SETTEN:
        for (unsigned ch = 0; ch < IMXRT1180_LPIT_NCHAN; ch++) {
            if (v & (1u << ch)) {
                s->tctrl[ch] |= TCTRL_T_EN;
                lpit_channel_set_enabled(s, ch, true);
            }
        }
        break;
    case LPIT_CLRTEN:
        for (unsigned ch = 0; ch < IMXRT1180_LPIT_NCHAN; ch++) {
            if (v & (1u << ch)) {
                s->tctrl[ch] &= ~TCTRL_T_EN;
                lpit_channel_set_enabled(s, ch, false);
            }
        }
        break;
    default:
        if (offset >= LPIT_CH_BASE) {
            unsigned ch = (offset - LPIT_CH_BASE) / LPIT_CH_STRIDE;
            unsigned reg = (offset - LPIT_CH_BASE) % LPIT_CH_STRIDE;
            if (ch < IMXRT1180_LPIT_NCHAN) {
                if (reg == LPIT_TVAL) {
                    s->tval[ch] = v;
                    return;
                } else if (reg == LPIT_TCTRL) {
                    bool was = s->tctrl[ch] & TCTRL_T_EN;
                    s->tctrl[ch] = v;
                    if ((v & TCTRL_T_EN) && !was) {
                        lpit_channel_set_enabled(s, ch, true);
                    } else if (!(v & TCTRL_T_EN) && was) {
                        lpit_channel_set_enabled(s, ch, false);
                    }
                    return;
                }
                return;   /* CVAL is read-only */
            }
        }
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @0x%02x=0x%08x\n",
                      __func__, (unsigned)offset, v);
        break;
    }
}

static const MemoryRegionOps imxrt1180_lpit_ops = {
    .read = imxrt1180_lpit_read,
    .write = imxrt1180_lpit_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_lpit_reset(DeviceState *dev)
{
    IMXRT1180LPITState *s = IMXRT1180_LPIT(dev);

    s->mcr = s->msr = s->mier = 0;
    for (unsigned ch = 0; ch < IMXRT1180_LPIT_NCHAN; ch++) {
        s->tval[ch] = s->tctrl[ch] = 0;
        ptimer_transaction_begin(s->timer[ch]);
        ptimer_stop(s->timer[ch]);
        ptimer_transaction_commit(s->timer[ch]);
    }
    qemu_set_irq(s->irq, 0);
}

static void imxrt1180_lpit_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180LPITState *s = IMXRT1180_LPIT(dev);

    if (s->freq == 0) {
        s->freq = LPIT_FREQ_DEFAULT;
    }
    for (unsigned ch = 0; ch < IMXRT1180_LPIT_NCHAN; ch++) {
        s->chan[ch].s = s;
        s->chan[ch].ch = ch;
        s->timer[ch] = ptimer_init(lpit_channel_tick, &s->chan[ch],
                                   PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                   PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_lpit_ops, s,
                          TYPE_IMXRT1180_LPIT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imxrt1180_lpit = {
    .name = TYPE_IMXRT1180_LPIT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mcr, IMXRT1180LPITState),
        VMSTATE_UINT32(msr, IMXRT1180LPITState),
        VMSTATE_UINT32(mier, IMXRT1180LPITState),
        VMSTATE_UINT32_ARRAY(tval, IMXRT1180LPITState, IMXRT1180_LPIT_NCHAN),
        VMSTATE_UINT32_ARRAY(tctrl, IMXRT1180LPITState, IMXRT1180_LPIT_NCHAN),
        VMSTATE_PTIMER_ARRAY(timer, IMXRT1180LPITState, IMXRT1180_LPIT_NCHAN),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_lpit_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_lpit_realize;
    device_class_set_legacy_reset(dc, imxrt1180_lpit_reset);
    dc->vmsd = &vmstate_imxrt1180_lpit;
}

static const TypeInfo imxrt1180_lpit_types[] = {
    {
        .name          = TYPE_IMXRT1180_LPIT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180LPITState),
        .class_init    = imxrt1180_lpit_class_init,
    },
};

DEFINE_TYPES(imxrt1180_lpit_types)
