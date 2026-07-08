/*
 * NXP i.MX RT1180 generic peripheral readiness block.  See the header.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/imxrt1180_periphrdy.h"
#include "migration/vmstate.h"

static uint64_t rdy_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180PeriphRdyState *s = opaque;
    if (off + 4 > s->mmsize) { return 0; }
    uint32_t v = s->regs[off / 4];
    if (off == s->rdy_off) { v |= s->rdy_mask; }
    return v;
}
static void rdy_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IMXRT1180PeriphRdyState *s = opaque;
    if (off + 4 <= s->mmsize) { s->regs[off / 4] = v; }
}
static const MemoryRegionOps rdy_ops = {
    .read = rdy_read, .write = rdy_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .impl.min_access_size = 4, .impl.max_access_size = 4,
};
static void rdy_reset(DeviceState *dev)
{
    IMXRT1180PeriphRdyState *s = IMXRT1180_PERIPHRDY(dev);
    memset(s->regs, 0, s->mmsize);
}
static void rdy_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180PeriphRdyState *s = IMXRT1180_PERIPHRDY(dev);
    if (!s->mmsize) { s->mmsize = 0x1000; }
    s->regs = g_malloc0(s->mmsize);
    memory_region_init_io(&s->iomem, OBJECT(s), &rdy_ops, s, TYPE_IMXRT1180_PERIPHRDY, s->mmsize);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}
static const Property rdy_props[] = {
    DEFINE_PROP_UINT32("mmsize", IMXRT1180PeriphRdyState, mmsize, 0),
    DEFINE_PROP_UINT32("rdy-off", IMXRT1180PeriphRdyState, rdy_off, 0xFFFFFFFF),
    DEFINE_PROP_UINT32("rdy-mask", IMXRT1180PeriphRdyState, rdy_mask, 0),
};
static void rdy_class_init(ObjectClass *k, const void *d)
{
    DeviceClass *dc = DEVICE_CLASS(k);
    dc->realize = rdy_realize; device_class_set_legacy_reset(dc, rdy_reset);
    device_class_set_props(dc, rdy_props);
}
static const TypeInfo rdy_types[] = {{ .name = TYPE_IMXRT1180_PERIPHRDY, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180PeriphRdyState), .class_init = rdy_class_init }};
DEFINE_TYPES(rdy_types)
