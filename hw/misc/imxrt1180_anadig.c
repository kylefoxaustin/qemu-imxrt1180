/*
 * NXP i.MX RT1180 ANADIG (analog clock: OSC + PLL + PMU) — clock-ready model.
 *
 * The SDK CLOCK_Init enables the 24 MHz OSC and each PLL, then polls their
 * "stable/locked" status bits.  In emulation clock lock is instantaneous, so
 * this model is register-backed (reads return what was written) and forces the
 * OSC-stable / PLL-stable status bits SET on read for the known status
 * registers — the standard QEMU clock-controller approach (cf. the i.MX / MCX
 * SCG "clocks ready" stubs).  It does NOT compute real clock frequencies.
 *
 * Register offsets and STABLE-bit masks VERIFIED against the MIMXRT1189 CMSIS
 * (PERI_ANADIG_OSC.h / PERI_ANADIG_PLL.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_anadig.h"
#include "migration/vmstate.h"

/* STABLE / lock status bits that firmware polls, per register offset. */
#define OSC_24M_STABLE   0x40000000u   /* OSC_24M_CTRL  @0x4320, bit 30 */
#define PLL_STABLE       0x20000000u   /* *_PLL_CTRL,           bit 29 */
#define PFD_STABLE_ALL   0x40404040u   /* SYS_PLLn_PFD PFD0..3 stable  */

static uint32_t imxrt1180_anadig_force_bits(hwaddr offset)
{
    switch (offset) {
    case 0x4320: return OSC_24M_STABLE;   /* OSC_24M_CTRL   */
    case 0x4000: return PLL_STABLE;       /* ARM_PLL_CTRL   */
    case 0x4010: return PLL_STABLE;       /* SYS_PLL3_CTRL  */
    case 0x4030: return PFD_STABLE_ALL;   /* SYS_PLL3_PFD   */
    case 0x4040: return PLL_STABLE;       /* SYS_PLL2_CTRL  */
    case 0x4070: return PFD_STABLE_ALL;   /* SYS_PLL2_PFD   */
    case 0x4100: return PLL_STABLE;       /* SYS_PLL1_CTRL  */
    case 0x4200: return PLL_STABLE;       /* PLL_AUDIO_CTRL */
    default:     return 0;
    }
}

static uint64_t imxrt1180_anadig_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180AnadigState *s = IMXRT1180_ANADIG(opaque);

    if (offset + 4 > IMXRT1180_ANADIG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
    return s->regs[offset / 4] | imxrt1180_anadig_force_bits(offset);
}

static void imxrt1180_anadig_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IMXRT1180AnadigState *s = IMXRT1180_ANADIG(opaque);

    if (offset + 4 > IMXRT1180_ANADIG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imxrt1180_anadig_ops = {
    .read = imxrt1180_anadig_read,
    .write = imxrt1180_anadig_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_anadig_reset(DeviceState *dev)
{
    IMXRT1180AnadigState *s = IMXRT1180_ANADIG(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imxrt1180_anadig_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180AnadigState *s = IMXRT1180_ANADIG(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_anadig_ops, s,
                          TYPE_IMXRT1180_ANADIG, IMXRT1180_ANADIG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imxrt1180_anadig = {
    .name = TYPE_IMXRT1180_ANADIG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180AnadigState,
                             IMXRT1180_ANADIG_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_anadig_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_anadig_realize;
    device_class_set_legacy_reset(dc, imxrt1180_anadig_reset);
    dc->vmsd = &vmstate_imxrt1180_anadig;
}

static const TypeInfo imxrt1180_anadig_types[] = {
    {
        .name          = TYPE_IMXRT1180_ANADIG,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180AnadigState),
        .class_init    = imxrt1180_anadig_class_init,
    },
};

DEFINE_TYPES(imxrt1180_anadig_types)
