/*
 * NXP i.MX RT1180 RTWDOG (Watchdog Timer) — register-accurate disable stub.
 *
 * The SDK SystemInit unlocks each RTWDOG (write 0xC520 then 0xD928 to CNT) and
 * reconfigures CS to disable it.  This model honours that sequence and reports
 * the unlock (ULK) and reconfiguration-success (RCS) status so the driver
 * proceeds; it does NOT model the countdown/reset (there is no watchdog bite in
 * emulation — flagged here rather than silently pretending to run).
 *
 * Register offsets/bits VERIFIED against the MIMXRT1189 CMSIS PERI_RTWDOG.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_rtwdog.h"
#include "migration/vmstate.h"

#define RTWDOG_CS     0x0
#define RTWDOG_CNT    0x4
#define RTWDOG_TOVAL  0x8
#define RTWDOG_WIN    0xC

#define CS_EN    0x00000080u
#define CS_RCS   0x00000400u   /* reconfiguration success (RO status) */
#define CS_ULK   0x00000800u   /* unlocked (RO status)                */

#define UNLOCK_KEY0  0xC520u
#define UNLOCK_KEY1  0xD928u

static uint64_t imxrt1180_rtwdog_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180RTWDOGState *s = IMXRT1180_RTWDOG(opaque);

    switch (offset) {
    case RTWDOG_CS:
        /* Report reconfig-success always, and unlocked while the window open. */
        return s->cs | CS_RCS | (s->unlocked ? CS_ULK : 0);
    case RTWDOG_CNT:
        return s->cnt;
    case RTWDOG_TOVAL:
        return s->toval;
    case RTWDOG_WIN:
        return s->win;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void imxrt1180_rtwdog_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IMXRT1180RTWDOGState *s = IMXRT1180_RTWDOG(opaque);

    switch (offset) {
    case RTWDOG_CS:
        s->cs = value & ~(CS_RCS | CS_ULK);  /* status bits are RO */
        break;
    case RTWDOG_CNT:
        /* Unlock sequence: 0xC520 then 0xD928 written to CNT. */
        if ((value & 0xFFFFu) == UNLOCK_KEY0) {
            s->unlock_step = 1;
        } else if (s->unlock_step == 1 && (value & 0xFFFFu) == UNLOCK_KEY1) {
            s->unlocked = true;
            s->unlock_step = 0;
        } else {
            s->unlock_step = 0;
        }
        break;
    case RTWDOG_TOVAL:
        s->toval = value;
        break;
    case RTWDOG_WIN:
        s->win = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled write @0x%" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, (uint32_t)value);
        break;
    }
}

static const MemoryRegionOps imxrt1180_rtwdog_ops = {
    .read = imxrt1180_rtwdog_read,
    .write = imxrt1180_rtwdog_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_rtwdog_reset(DeviceState *dev)
{
    IMXRT1180RTWDOGState *s = IMXRT1180_RTWDOG(dev);

    s->cs = 0;
    s->cnt = 0;
    s->toval = 0;
    s->win = 0;
    s->unlock_step = 0;
    s->unlocked = false;
}

static void imxrt1180_rtwdog_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180RTWDOGState *s = IMXRT1180_RTWDOG(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_rtwdog_ops, s,
                          TYPE_IMXRT1180_RTWDOG, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imxrt1180_rtwdog = {
    .name = TYPE_IMXRT1180_RTWDOG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cs, IMXRT1180RTWDOGState),
        VMSTATE_UINT32(cnt, IMXRT1180RTWDOGState),
        VMSTATE_UINT32(toval, IMXRT1180RTWDOGState),
        VMSTATE_UINT32(win, IMXRT1180RTWDOGState),
        VMSTATE_UINT8(unlock_step, IMXRT1180RTWDOGState),
        VMSTATE_BOOL(unlocked, IMXRT1180RTWDOGState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_rtwdog_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_rtwdog_realize;
    device_class_set_legacy_reset(dc, imxrt1180_rtwdog_reset);
    dc->vmsd = &vmstate_imxrt1180_rtwdog;
}

static const TypeInfo imxrt1180_rtwdog_types[] = {
    {
        .name          = TYPE_IMXRT1180_RTWDOG,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180RTWDOGState),
        .class_init    = imxrt1180_rtwdog_class_init,
    },
};

DEFINE_TYPES(imxrt1180_rtwdog_types)
