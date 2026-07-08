/*
 * NXP i.MX RT1180 MSGINTR — Message-Signalled Interrupt router.
 *
 * Register map (per channel c, MSGINTR_MSI_COUNT=3): MSI[c].MSIIR at c*8 (write-
 * only index), MSI[c].MSIR at c*8+4 (read, self-clearing).  Writing index N to
 * MSIIR sets bit (1<<N) in MSIR[c] and raises the shared NVIC line; the ISR
 * reads MSIR[c] to consume + clear the pending bits.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/intc/imxrt1180_msgintr.h"
#include "migration/vmstate.h"

static void msgintr_update(IMXRT1180MSGINTRState *s)
{
    uint32_t any = 0;
    for (int c = 0; c < IMXRT1180_MSGINTR_CHANNELS; c++) {
        any |= s->pending[c];
    }
    qemu_set_irq(s->irq, !!any);
}

static uint64_t msgintr_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180MSGINTRState *s = opaque;
    unsigned ch = off / 8;

    if ((off & 0x4) && ch < IMXRT1180_MSGINTR_CHANNELS) {
        /* MSIR read: return the pending bits and clear them. */
        uint32_t v = s->pending[ch];
        s->pending[ch] = 0;
        msgintr_update(s);
        return v;
    }
    return 0;   /* MSIIR is write-only */
}

static void msgintr_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IMXRT1180MSGINTRState *s = opaque;
    unsigned ch = off / 8;

    if (!(off & 0x4) && ch < IMXRT1180_MSGINTR_CHANNELS) {
        /* MSIIR write: index N -> set pending bit (1<<N). */
        s->pending[ch] |= 1u << (val & 0x1F);
        msgintr_update(s);
    }
}

static const MemoryRegionOps msgintr_ops = {
    .read = msgintr_read,
    .write = msgintr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void msgintr_reset(DeviceState *dev)
{
    IMXRT1180MSGINTRState *s = IMXRT1180_MSGINTR(dev);
    memset(s->pending, 0, sizeof(s->pending));
    qemu_set_irq(s->irq, 0);
}

static void msgintr_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180MSGINTRState *s = IMXRT1180_MSGINTR(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &msgintr_ops, s,
                          TYPE_IMXRT1180_MSGINTR, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_msgintr = {
    .name = TYPE_IMXRT1180_MSGINTR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(pending, IMXRT1180MSGINTRState,
                             IMXRT1180_MSGINTR_CHANNELS),
        VMSTATE_END_OF_LIST()
    },
};

static void msgintr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = msgintr_realize;
    device_class_set_legacy_reset(dc, msgintr_reset);
    dc->vmsd = &vmstate_msgintr;
}

static const TypeInfo msgintr_types[] = {
    {
        .name = TYPE_IMXRT1180_MSGINTR,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180MSGINTRState),
        .class_init = msgintr_class_init,
    },
};
DEFINE_TYPES(msgintr_types)
