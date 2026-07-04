/*
 * NXP i.MX RT1180 TRDC — Trusted Resource Domain Controller (config stub).
 *
 * The TRDC gates bus masters/regions by security domain.  SDK drivers (eDMA,
 * SAI, ...) read TRDC_HWCFG0 to size their domain/master loops and ASSERT if
 * the master/domain counts read back zero.  This stub is register-backed and
 * returns a sane hardware-config (non-zero master/domain/region counts) so
 * those drivers proceed; it does NOT enforce any access control (every access
 * is already permitted in this model — flagged, not silently pretending to
 * restrict).
 *
 * HWCFG0 @0xF0 fields verified against the MIMXRT1189 CMSIS PERI_TRDC.h:
 * NDID[4:0], NMSTR[15:8], NMRC[28:24].
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_trdc.h"
#include "migration/vmstate.h"

#define TRDC_HWCFG0   0xF0
/* NDID=4 domains, NMSTR=16 masters, NMRC=4 memory-region-check blocks. */
#define TRDC_HWCFG0_VALUE  ((4u) | (16u << 8) | (4u << 24))

static uint64_t imxrt1180_trdc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180TRDCState *s = IMXRT1180_TRDC(opaque);

    if (offset + 4 > IMXRT1180_TRDC_SIZE) {
        return 0;
    }
    if (offset == TRDC_HWCFG0) {
        return TRDC_HWCFG0_VALUE;
    }
    return s->regs[offset / 4];
}

static void imxrt1180_trdc_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    IMXRT1180TRDCState *s = IMXRT1180_TRDC(opaque);

    if (offset + 4 > IMXRT1180_TRDC_SIZE) {
        return;
    }
    if (offset == TRDC_HWCFG0) {
        return;   /* read-only */
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imxrt1180_trdc_ops = {
    .read = imxrt1180_trdc_read,
    .write = imxrt1180_trdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_trdc_reset(DeviceState *dev)
{
    IMXRT1180TRDCState *s = IMXRT1180_TRDC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imxrt1180_trdc_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180TRDCState *s = IMXRT1180_TRDC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_trdc_ops, s,
                          TYPE_IMXRT1180_TRDC, IMXRT1180_TRDC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imxrt1180_trdc = {
    .name = TYPE_IMXRT1180_TRDC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180TRDCState, IMXRT1180_TRDC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_trdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_trdc_realize;
    device_class_set_legacy_reset(dc, imxrt1180_trdc_reset);
    dc->vmsd = &vmstate_imxrt1180_trdc;
}

static const TypeInfo imxrt1180_trdc_types[] = {
    {
        .name          = TYPE_IMXRT1180_TRDC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180TRDCState),
        .class_init    = imxrt1180_trdc_class_init,
    },
};

DEFINE_TYPES(imxrt1180_trdc_types)
