/*
 * NXP i.MX RT1180 MU — inter-core Messaging Unit (CM33 <-> CM7).
 *
 * Register-accurate against the MIMXRT1189 CMSIS PERI_MU.h.  Models both sides
 * (MUA/MUB) of one MU and cross-wires them: MUA.TR[n] -> MUB.RR[n], the GCR/GSR
 * doorbell crosses cores, and each side's aggregated interrupt drives its NVIC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/misc/imxrt1180_mu.h"
#include "migration/vmstate.h"

/* Register offsets (both sides identical). */
#define MU_VER   0x000   /* Version (RO) */
#define MU_PAR   0x004   /* Parameter (RO) */
#define MU_CR    0x008   /* Control */
#define MU_SR    0x00C   /* Status (RO, aggregated) */
#define MU_FCR   0x100   /* Flag Control */
#define MU_FSR   0x104   /* Flag Status (RO) */
#define MU_GIER  0x110   /* GP Interrupt Enable */
#define MU_GCR   0x114   /* GP Interrupt Request */
#define MU_GSR   0x118   /* GP Interrupt Status/pending (W1C) */
#define MU_TCR   0x120   /* Transmit Interrupt Enable */
#define MU_TSR   0x124   /* Transmit Status (RO) */
#define MU_RCR   0x128   /* Receive Interrupt Enable */
#define MU_RSR   0x12C   /* Receive Status (RO) */
#define MU_TR0   0x200   /* Transmit[0..3] */
#define MU_RR0   0x280   /* Receive[0..3] */

#define MU_CHAN_MASK ((1u << IMXRT1180_MU_NCHAN) - 1)  /* 0xF */

/* PAR: 4 TR, 4 RR, 4 GIR, 3 flags. */
#define MU_PAR_VALUE \
    (IMXRT1180_MU_NCHAN | (IMXRT1180_MU_NCHAN << 8) | \
     (IMXRT1180_MU_NCHAN << 16) | (3u << 24))
#define MU_VER_VALUE 0x00000300

/* SR aggregate flag bits (subset we drive). */
#define MU_SR_GIRP  0x10
#define MU_SR_TEP   0x20
#define MU_SR_RFP   0x40

/* TSR.TEn for side s: channel n is empty (ready to send) when not pending. */
static inline uint32_t mu_tsr(IMXRT1180MUState *s, unsigned side)
{
    return (~s->full[side]) & MU_CHAN_MASK;
}

/* RSR.RFn for side s: channel n is full when the other side left it pending. */
static inline uint32_t mu_rsr(IMXRT1180MUState *s, unsigned side)
{
    return s->full[side ^ 1] & MU_CHAN_MASK;
}

static void imxrt1180_mu_update_irq(IMXRT1180MUState *s, unsigned side)
{
    uint32_t pending = (mu_rsr(s, side) & s->rcr[side]) |
                       (mu_tsr(s, side) & s->tcr[side]) |
                       (s->gsr[side]    & s->gier[side]);
    qemu_set_irq(s->irq[side], pending != 0);
}

static void imxrt1180_mu_reset_side(IMXRT1180MUState *s, unsigned side)
{
    s->cr[side] = s->tcr[side] = s->rcr[side] = 0;
    s->gier[side] = s->gcr[side] = s->gsr[side] = s->fcr[side] = 0;
    s->full[side] = 0;
    for (int n = 0; n < IMXRT1180_MU_NCHAN; n++) {
        s->chan[side][n] = 0;
    }
}

static uint64_t imxrt1180_mu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180MUPort *p = opaque;
    IMXRT1180MUState *s = p->mu;
    unsigned side = p->side, other = side ^ 1;

    switch (offset) {
    case MU_VER:
        return MU_VER_VALUE;
    case MU_PAR:
        return MU_PAR_VALUE;
    case MU_CR:
        return s->cr[side];
    case MU_SR:
        return ((mu_rsr(s, side) & s->rcr[side]) ? MU_SR_RFP : 0) |
               ((mu_tsr(s, side) & s->tcr[side]) ? MU_SR_TEP : 0) |
               ((s->gsr[side] & s->gier[side])   ? MU_SR_GIRP : 0);
    case MU_FCR:
        return s->fcr[side];
    case MU_FSR:
        return s->fcr[other];            /* Fn flags set by the other side */
    case MU_GIER:
        return s->gier[side];
    case MU_GCR:
        return s->gcr[side];
    case MU_GSR:
        return s->gsr[side];
    case MU_TCR:
        return s->tcr[side];
    case MU_TSR:
        return mu_tsr(s, side);
    case MU_RCR:
        return s->rcr[side];
    case MU_RSR:
        return mu_rsr(s, side);
    case MU_TR0 ... MU_TR0 + 4 * (IMXRT1180_MU_NCHAN - 1):
        return s->chan[side][(offset - MU_TR0) / 4];
    case MU_RR0 ... MU_RR0 + 4 * (IMXRT1180_MU_NCHAN - 1): {
        unsigned n = (offset - MU_RR0) / 4;
        uint32_t val = s->chan[other][n];
        if (s->full[other] & (1u << n)) {
            s->full[other] &= ~(1u << n);   /* consume: RR emptied */
            imxrt1180_mu_update_irq(s, side);   /* our RSR.RF fell */
            imxrt1180_mu_update_irq(s, other);  /* other's TSR.TE rose */
        }
        return val;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @0x%03x (side %c)\n",
                      __func__, (unsigned)offset, side ? 'B' : 'A');
        return 0;
    }
}

static void imxrt1180_mu_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    IMXRT1180MUPort *p = opaque;
    IMXRT1180MUState *s = p->mu;
    unsigned side = p->side, other = side ^ 1;
    uint32_t v = value;

    switch (offset) {
    case MU_CR:
        s->cr[side] = v & 0x3;               /* MUR | MURIE */
        if (v & 0x1) {                        /* MUR: reset this side */
            imxrt1180_mu_reset_side(s, side);
        }
        imxrt1180_mu_update_irq(s, side);
        break;
    case MU_FCR:
        s->fcr[side] = v;                     /* Fn -> other side's FSR */
        break;
    case MU_GIER:
        s->gier[side] = v & MU_CHAN_MASK;
        imxrt1180_mu_update_irq(s, side);
        break;
    case MU_GCR: {
        /* Write 1 to GIRn triggers a GP interrupt on the OTHER side; the bit
         * stays set until that side clears its GSR.GIPn (handshake). */
        uint32_t newbits = v & MU_CHAN_MASK & ~s->gcr[side];
        s->gcr[side] |= v & MU_CHAN_MASK;
        if (newbits) {
            s->gsr[other] |= newbits;
            imxrt1180_mu_update_irq(s, other);
        }
        break;
    }
    case MU_GSR:
        /* W1C: clearing a pending GP interrupt also releases the requester. */
        s->gsr[side] &= ~(v & MU_CHAN_MASK);
        s->gcr[other] &= ~(v & MU_CHAN_MASK);
        imxrt1180_mu_update_irq(s, side);
        break;
    case MU_TCR:
        s->tcr[side] = v & MU_CHAN_MASK;
        imxrt1180_mu_update_irq(s, side);
        break;
    case MU_RCR:
        s->rcr[side] = v & MU_CHAN_MASK;
        imxrt1180_mu_update_irq(s, side);
        break;
    case MU_TR0 ... MU_TR0 + 4 * (IMXRT1180_MU_NCHAN - 1): {
        unsigned n = (offset - MU_TR0) / 4;
        s->chan[side][n] = v;
        s->full[side] |= (1u << n);          /* now pending in other's RR[n] */
        imxrt1180_mu_update_irq(s, side);    /* our TSR.TE fell */
        imxrt1180_mu_update_irq(s, other);   /* other's RSR.RF rose */
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @0x%03x=0x%08x "
                      "(side %c)\n", __func__, (unsigned)offset, v,
                      side ? 'B' : 'A');
        break;
    }
}

static const MemoryRegionOps imxrt1180_mu_ops = {
    .read = imxrt1180_mu_read,
    .write = imxrt1180_mu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_mu_reset(DeviceState *dev)
{
    IMXRT1180MUState *s = IMXRT1180_MU(dev);

    imxrt1180_mu_reset_side(s, 0);
    imxrt1180_mu_reset_side(s, 1);
    qemu_set_irq(s->irq[0], 0);
    qemu_set_irq(s->irq[1], 0);
}

static void imxrt1180_mu_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180MUState *s = IMXRT1180_MU(dev);
    static const char *const names[2] = { "imxrt1180.mu.mua",
                                          "imxrt1180.mu.mub" };

    for (unsigned side = 0; side < 2; side++) {
        s->port[side].mu = s;
        s->port[side].side = side;
        memory_region_init_io(&s->iomem[side], OBJECT(s), &imxrt1180_mu_ops,
                              &s->port[side], names[side], 0x1000);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem[side]);
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[side]);
    }
}

/* Migration: re-derive IRQ line levels from restored state (outputs are
 * not migrated, so a pending per-unit IRQ would be lost). */
static int vmstate_imxrt1180_mu_post_load(void *opaque, int version_id)
{
    IMXRT1180MUState *s = opaque;
    for (unsigned side = 0; side < 2; side++) {
        imxrt1180_mu_update_irq(s, side);
    }
    return 0;
}

static const VMStateDescription vmstate_imxrt1180_mu = {
    .name = TYPE_IMXRT1180_MU,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = vmstate_imxrt1180_mu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(chan, IMXRT1180MUState, 2, IMXRT1180_MU_NCHAN),
        VMSTATE_UINT8_ARRAY(full, IMXRT1180MUState, 2),
        VMSTATE_UINT32_ARRAY(cr, IMXRT1180MUState, 2),
        VMSTATE_UINT32_ARRAY(tcr, IMXRT1180MUState, 2),
        VMSTATE_UINT32_ARRAY(rcr, IMXRT1180MUState, 2),
        VMSTATE_UINT32_ARRAY(gier, IMXRT1180MUState, 2),
        VMSTATE_UINT32_ARRAY(gcr, IMXRT1180MUState, 2),
        VMSTATE_UINT32_ARRAY(gsr, IMXRT1180MUState, 2),
        VMSTATE_UINT32_ARRAY(fcr, IMXRT1180MUState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_mu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_mu_realize;
    device_class_set_legacy_reset(dc, imxrt1180_mu_reset);
    dc->vmsd = &vmstate_imxrt1180_mu;
}

static const TypeInfo imxrt1180_mu_types[] = {
    {
        .name          = TYPE_IMXRT1180_MU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180MUState),
        .class_init    = imxrt1180_mu_class_init,
    },
};

DEFINE_TYPES(imxrt1180_mu_types)
