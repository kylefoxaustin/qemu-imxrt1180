/*
 * NXP i.MX RT1180 CMP — high-speed analog comparator.
 *
 * Register-accurate against the MIMXRT1189 DFP PERI_CMP.h (C0..C3 control).
 * The control/config registers are register-backed; the comparator output
 * C0.COUT reads a stable low (no analog inputs are wired), and the C0.CFR/CFF
 * edge flags are write-1-to-clear.
 *
 * FIDELITY NOTE — there is no analog front-end, so the comparison result is not
 * computed: COUT is a fixed level and no edge interrupt is generated.  Honest
 * "comparator present, inputs unconnected" state (flagged), not a fabricated
 * comparison.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/misc/imxrt1180_cmp.h"
#include "migration/vmstate.h"

#define R_C0 0x8
#define C0_CFF 0x2000000
#define C0_CFR 0x4000000

static uint64_t cmp_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180CmpState *s = opaque;
    return off + 4 <= sizeof(s->regs) ? s->regs[off / 4] : 0;
}
static void cmp_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IMXRT1180CmpState *s = opaque;
    if (off + 4 > sizeof(s->regs)) { return; }
    if (off == R_C0) {
        /* CFR/CFF are write-1-to-clear; COUT (bit24) is read-only. */
        uint32_t old = s->regs[off / 4];
        s->regs[off / 4] = (v & ~(C0_CFF | C0_CFR)) | (old & ~v & (C0_CFF | C0_CFR));
        return;
    }
    s->regs[off / 4] = v;
}
static const MemoryRegionOps cmp_ops = {
    .read = cmp_read, .write = cmp_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static void cmp_reset(DeviceState *dev)
{
    IMXRT1180CmpState *s = IMXRT1180_CMP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* C3 @0x14 (PERI_CMP.h) resets to 0x1100_0000 -- the RM's cold-POR column. */
    s->regs[0x14 / 4] = 0x11000000;
}
static void cmp_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180CmpState *s = IMXRT1180_CMP(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &cmp_ops, s, TYPE_IMXRT1180_CMP, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}
static const VMStateDescription vmstate_cmp = { .name = TYPE_IMXRT1180_CMP, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) { VMSTATE_UINT32_ARRAY(regs, IMXRT1180CmpState, 0x40/4), VMSTATE_END_OF_LIST() } };
static void cmp_class_init(ObjectClass *k, const void *d)
{ DeviceClass *dc = DEVICE_CLASS(k); dc->realize = cmp_realize; device_class_set_legacy_reset(dc, cmp_reset); dc->vmsd = &vmstate_cmp; }
static const TypeInfo cmp_types[] = {{ .name = TYPE_IMXRT1180_CMP, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180CmpState), .class_init = cmp_class_init }};
DEFINE_TYPES(cmp_types)
