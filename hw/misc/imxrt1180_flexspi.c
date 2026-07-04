/*
 * NXP i.MX RT1180 FlexSPI — controller readiness model.
 *
 * Board init pokes the FlexSPI (soft reset + config) and polls STS0 for the
 * controller to go idle.  This model is register-backed and reports the
 * controller permanently idle (STS0.SEQIDLE|ARBIDLE) with a self-clearing
 * MCR0.SWRESET, which is what the config/poll code needs.  It does NOT model
 * the serial-flash command engine, the LUT, or the AHB-mapped XIP flash window
 * (a separate concern, needed only to execute FlexSPI-NOR boot images) — those
 * are flagged as future work, not silently faked.
 *
 * Register offsets/bits verified against the MIMXRT1189 CMSIS PERI_FLEXSPI.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_flexspi.h"
#include "migration/vmstate.h"

#define FLEXSPI_MCR0    0x00
#define FLEXSPI_INTR    0x14
#define FLEXSPI_STS0    0xE0

#define MCR0_SWRESET    0x00000001u
#define STS0_SEQIDLE    0x00000001u
#define STS0_ARBIDLE    0x00000002u

static uint64_t imxrt1180_flexspi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180FlexSPIState *s = IMXRT1180_FLEXSPI(opaque);

    if (offset + 4 > IMXRT1180_FLEXSPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    switch (offset) {
    case FLEXSPI_STS0:
        /* Controller always idle (no command in flight in this model). */
        return STS0_SEQIDLE | STS0_ARBIDLE;
    case FLEXSPI_MCR0:
        /* SWRESET is self-clearing: report the reset already complete. */
        return s->regs[FLEXSPI_MCR0 / 4] & ~MCR0_SWRESET;
    default:
        return s->regs[offset / 4];
    }
}

static void imxrt1180_flexspi_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    IMXRT1180FlexSPIState *s = IMXRT1180_FLEXSPI(opaque);

    if (offset + 4 > IMXRT1180_FLEXSPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    if (offset == FLEXSPI_INTR) {
        s->regs[offset / 4] &= ~(uint32_t)value;   /* W1C */
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imxrt1180_flexspi_ops = {
    .read = imxrt1180_flexspi_read,
    .write = imxrt1180_flexspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_flexspi_reset(DeviceState *dev)
{
    IMXRT1180FlexSPIState *s = IMXRT1180_FLEXSPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imxrt1180_flexspi_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180FlexSPIState *s = IMXRT1180_FLEXSPI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_flexspi_ops, s,
                          TYPE_IMXRT1180_FLEXSPI, IMXRT1180_FLEXSPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imxrt1180_flexspi = {
    .name = TYPE_IMXRT1180_FLEXSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180FlexSPIState,
                             IMXRT1180_FLEXSPI_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_flexspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_flexspi_realize;
    device_class_set_legacy_reset(dc, imxrt1180_flexspi_reset);
    dc->vmsd = &vmstate_imxrt1180_flexspi;
}

static const TypeInfo imxrt1180_flexspi_types[] = {
    {
        .name          = TYPE_IMXRT1180_FLEXSPI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180FlexSPIState),
        .class_init    = imxrt1180_flexspi_class_init,
    },
};

DEFINE_TYPES(imxrt1180_flexspi_types)
