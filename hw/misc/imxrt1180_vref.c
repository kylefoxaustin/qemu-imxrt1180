/*
 * NXP i.MX RT1180 VREF — voltage reference.
 *
 * Register-accurate against the MIMXRT1189 DFP PERI_VREF.h.  Config registers
 * are register-backed; CSR reports the internal reference stable/ready as soon
 * as it is enabled (VREFEN), so firmware's "wait for VREF stable" completes.
 * There is no analog output to model (flagged).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/imxrt1180_vref.h"
#include "migration/vmstate.h"

#define R_CSR 0x8
#define CSR_VREFEN 0x1
#define CSR_VREFST 0x4        /* internal voltage reference stable */

static uint64_t vref_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180VrefState *s = opaque;
    if (off + 4 > sizeof(s->regs)) { return 0; }
    uint32_t v = s->regs[off / 4];
    if (off == R_CSR && (v & CSR_VREFEN)) {
        v |= CSR_VREFST;                  /* stable once enabled */
    }
    return v;
}
static void vref_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IMXRT1180VrefState *s = opaque;
    if (off + 4 <= sizeof(s->regs)) { s->regs[off / 4] = v; }
}
static const MemoryRegionOps vref_ops = {
    .read = vref_read, .write = vref_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static void vref_reset(DeviceState *dev) { memset(IMXRT1180_VREF(dev)->regs, 0, sizeof(IMXRT1180_VREF(dev)->regs)); }
static void vref_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180VrefState *s = IMXRT1180_VREF(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &vref_ops, s, TYPE_IMXRT1180_VREF, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}
static const VMStateDescription vmstate_vref = { .name = TYPE_IMXRT1180_VREF, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) { VMSTATE_UINT32_ARRAY(regs, IMXRT1180VrefState, 0x40/4), VMSTATE_END_OF_LIST() } };
static void vref_class_init(ObjectClass *k, const void *d)
{ DeviceClass *dc = DEVICE_CLASS(k); dc->realize = vref_realize; device_class_set_legacy_reset(dc, vref_reset); dc->vmsd = &vmstate_vref; }
static const TypeInfo vref_types[] = {{ .name = TYPE_IMXRT1180_VREF, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180VrefState), .class_init = vref_class_init }};
DEFINE_TYPES(vref_types)
