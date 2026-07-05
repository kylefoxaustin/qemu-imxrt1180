/*
 * NXP MCX N eDMA (enhanced DMA) — functional model.  See header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/dma/imxrt1180_edma.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/* Management-page registers. */
#define R_MP_CSR  0x00
#define R_MP_ES   0x04
#define R_MP_INT  0x08
#define R_MP_HRS  0x0C
#define R_CH_GRPRI 0x100   /* [16] */

/* Per-channel register offsets (within the channel's 0x1000 block). */
#define R_CH_CSR   0x00
#define R_CH_ES    0x04
#define R_CH_INT   0x08
#define R_CH_SBR   0x0C
#define R_CH_PRI   0x10
#define R_CH_MUX   0x14
#define R_TCD_SADDR 0x20
#define R_TCD_SOFF  0x24
#define R_TCD_ATTR  0x26
#define R_TCD_NBYTES 0x28
#define R_TCD_SLAST 0x2C
#define R_TCD_DADDR 0x30
#define R_TCD_DOFF  0x34
#define R_TCD_CITER 0x36
#define R_TCD_DLAST 0x38
#define R_TCD_CSR   0x3C
#define R_TCD_BITER 0x3E

#define CH_CSR_ERQ   (1u << 0)
#define CH_CSR_DONE  (1u << 30)
#define CH_INT_INT   (1u << 0)
#define TCD_CSR_START    (1u << 0)
#define TCD_CSR_INTMAJOR (1u << 1)
#define ATTR_SSIZE(a)  (((a) >> 8) & 0x7)
#define ATTR_DSIZE(a)  ((a) & 0x7)
#define NBYTES_MASK  0x3FFFFFFFu
#define CITER_MASK   0x7FFFu

static void edma_update_irq(IMXRT1180EDMAState *s, int n)
{
    qemu_set_irq(s->irq[n], !!(s->ch[n].intr & CH_INT_INT));
}

/* Run a software-triggered channel transfer to completion. */
static void edma_run(IMXRT1180EDMAState *s, int n)
{
    IMXRT1180EDMAChan *c = &s->ch[n];
    uint32_t ssize = 1u << ATTR_SSIZE(c->tcd_attr);
    uint32_t dsize = 1u << ATTR_DSIZE(c->tcd_attr);
    uint32_t nbytes = c->tcd_nbytes & NBYTES_MASK;
    uint32_t citer = c->tcd_citer & CITER_MASK;
    int16_t soff = (int16_t)c->tcd_soff;
    int16_t doff = (int16_t)c->tcd_doff;
    uint32_t saddr = c->tcd_saddr, daddr = c->tcd_daddr;
    uint8_t buf[32];
    uint32_t step = ssize ? ssize : 1;
    uint32_t m, b;

    if (dsize == 0 || nbytes == 0 || citer == 0) {
        c->csr |= CH_CSR_DONE;
        return;
    }
    if (step > sizeof(buf)) {
        step = sizeof(buf);
    }
    for (m = 0; m < citer; m++) {
        for (b = 0; b + step <= nbytes; b += step) {
            address_space_read(&address_space_memory, saddr,
                               MEMTXATTRS_UNSPECIFIED, buf, step);
            address_space_write(&address_space_memory, daddr,
                                MEMTXATTRS_UNSPECIFIED, buf, step);
            saddr += soff;
            daddr += doff;
        }
    }
    saddr += (int32_t)c->tcd_slast;
    daddr += (int32_t)c->tcd_dlast;
    c->tcd_saddr = saddr;
    c->tcd_daddr = daddr;
    c->tcd_citer = c->tcd_biter;          /* reload major count */
    c->csr |= CH_CSR_DONE;
    if (c->tcd_csr & TCD_CSR_INTMAJOR) {
        c->intr |= CH_INT_INT;
        edma_update_irq(s, n);
    }
}

static uint64_t edma_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180EDMAState *s = IMXRT1180_EDMA(opaque);
    IMXRT1180EDMAChan *c;
    int n;

    if (off < 0x1000) {
        switch (off) {
        case R_MP_CSR: return s->mp_csr;
        case R_MP_ES:  return s->mp_es;
        case R_MP_INT: {
            uint32_t r = 0;
            for (n = 0; n < s->num_channels && n < 32; n++) {
                if (s->ch[n].intr & CH_INT_INT) {
                    r |= (1u << n);
                }
            }
            return r;
        }
        case R_MP_HRS: return 0;
        default:
            if (off >= R_CH_GRPRI && off < R_CH_GRPRI + 4 * s->num_channels) {
                return s->ch_grpri[(off - R_CH_GRPRI) / 4];
            }
            return 0;
        }
    }

    n = (off / 0x1000) - 1;
    if (n >= s->num_channels) {
        return 0;
    }
    c = &s->ch[n];
    switch (off % 0x1000) {
    case R_CH_CSR:    return c->csr;
    case R_CH_ES:     return c->es;
    case R_CH_INT:    return c->intr;
    case R_CH_SBR:    return c->sbr;
    case R_CH_PRI:    return c->pri;
    case R_CH_MUX:    return c->mux;
    case R_TCD_SADDR: return c->tcd_saddr;
    case R_TCD_SOFF:  return c->tcd_soff;
    case R_TCD_ATTR:  return c->tcd_attr;
    case R_TCD_NBYTES: return c->tcd_nbytes;
    case R_TCD_SLAST: return c->tcd_slast;
    case R_TCD_DADDR: return c->tcd_daddr;
    case R_TCD_DOFF:  return c->tcd_doff;
    case R_TCD_CITER: return c->tcd_citer;
    case R_TCD_DLAST: return c->tcd_dlast;
    case R_TCD_CSR:   return c->tcd_csr;
    case R_TCD_BITER: return c->tcd_biter;
    default:          return 0;
    }
}

static void edma_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IMXRT1180EDMAState *s = IMXRT1180_EDMA(opaque);
    IMXRT1180EDMAChan *c;
    uint32_t v = val;
    int n;

    if (off < 0x1000) {
        switch (off) {
        case R_MP_CSR: s->mp_csr = v; return;
        default:
            if (off >= R_CH_GRPRI && off < R_CH_GRPRI + 4 * s->num_channels) {
                s->ch_grpri[(off - R_CH_GRPRI) / 4] = v;
            }
            return;
        }
    }

    n = (off / 0x1000) - 1;
    if (n >= s->num_channels) {
        return;
    }
    c = &s->ch[n];
    switch (off % 0x1000) {
    case R_CH_CSR:
        /* DONE is write-1-to-clear; keep the rest. */
        if (v & CH_CSR_DONE) {
            c->csr &= ~CH_CSR_DONE;
        }
        c->csr = (c->csr & CH_CSR_DONE) | (v & ~CH_CSR_DONE);
        return;
    case R_CH_INT:
        if (v & CH_INT_INT) {                 /* write-1-to-clear */
            c->intr &= ~CH_INT_INT;
            edma_update_irq(s, n);
        }
        return;
    case R_CH_ES:   c->es = v; return;
    case R_CH_SBR:  c->sbr = v; return;
    case R_CH_PRI:  c->pri = v; return;
    case R_CH_MUX:  c->mux = v; return;
    case R_TCD_SADDR: c->tcd_saddr = v; return;
    case R_TCD_SOFF:  c->tcd_soff = v; return;
    case R_TCD_ATTR:  c->tcd_attr = v; return;
    case R_TCD_NBYTES: c->tcd_nbytes = v; return;
    case R_TCD_SLAST: c->tcd_slast = v; return;
    case R_TCD_DADDR: c->tcd_daddr = v; return;
    case R_TCD_DOFF:  c->tcd_doff = v; return;
    case R_TCD_CITER: c->tcd_citer = v; return;
    case R_TCD_DLAST: c->tcd_dlast = v; return;
    case R_TCD_CSR:
        c->tcd_csr = v;
        if (v & TCD_CSR_START) {
            c->csr &= ~CH_CSR_DONE;
            edma_run(s, n);
        }
        return;
    case R_TCD_BITER: c->tcd_biter = v; return;
    default: return;
    }
}

static const MemoryRegionOps edma_ops = {
    .read = edma_read,
    .write = edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void imxrt1180_edma_reset(DeviceState *dev)
{
    IMXRT1180EDMAState *s = IMXRT1180_EDMA(dev);

    s->mp_csr = 0;
    s->mp_es = 0;
    memset(s->ch_grpri, 0, sizeof(s->ch_grpri));
    memset(s->ch, 0, sizeof(s->ch));
}

static void imxrt1180_edma_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180EDMAState *s = IMXRT1180_EDMA(dev);
    int n;

    if (s->num_channels == 0 || s->num_channels > IMXRT1180_EDMA_MAX_CHANNELS) {
        s->num_channels = IMXRT1180_EDMA_MAX_CHANNELS;
    }
    /* Management page (0x0) + num_channels x 0x1000 blocks. */
    memory_region_init_io(&s->iomem, OBJECT(s), &edma_ops, s, TYPE_IMXRT1180_EDMA,
                          0x1000 * (s->num_channels + 1));
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (n = 0; n < s->num_channels; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[n]);
    }
}

static const VMStateDescription vmstate_edma_chan = {
    .name = "imxrt1180-edma-chan",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(csr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(es, IMXRT1180EDMAChan),
        VMSTATE_UINT32(intr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(sbr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(pri, IMXRT1180EDMAChan),
        VMSTATE_UINT32(mux, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_saddr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_slast, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_daddr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_dlast, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_nbytes, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_soff, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_attr, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_doff, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_citer, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_csr, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_biter, IMXRT1180EDMAChan),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_imxrt1180_edma = {
    .name = TYPE_IMXRT1180_EDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mp_csr, IMXRT1180EDMAState),
        VMSTATE_UINT32(mp_es, IMXRT1180EDMAState),
        VMSTATE_UINT32_ARRAY(ch_grpri, IMXRT1180EDMAState, IMXRT1180_EDMA_MAX_CHANNELS),
        VMSTATE_STRUCT_ARRAY(ch, IMXRT1180EDMAState, IMXRT1180_EDMA_MAX_CHANNELS, 1,
                             vmstate_edma_chan, IMXRT1180EDMAChan),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imxrt1180_edma_props[] = {
    DEFINE_PROP_UINT32("num-channels", IMXRT1180EDMAState, num_channels, 32),
};

static void imxrt1180_edma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_edma_realize;
    device_class_set_legacy_reset(dc, imxrt1180_edma_reset);
    device_class_set_props(dc, imxrt1180_edma_props);
    dc->vmsd = &vmstate_imxrt1180_edma;
}

static const TypeInfo imxrt1180_edma_types[] = {
    {
        .name          = TYPE_IMXRT1180_EDMA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180EDMAState),
        .class_init    = imxrt1180_edma_class_init,
    },
};

DEFINE_TYPES(imxrt1180_edma_types)
