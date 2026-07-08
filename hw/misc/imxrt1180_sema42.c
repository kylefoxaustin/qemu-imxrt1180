/*
 * NXP i.MX RT1180 SEMA42 — hardware semaphores.
 *
 * 16 one-byte gates.  Writing a nonzero domain value (master+1) to a free gate
 * (0) locks it to that value; writing 0 frees it; a write to an already-locked
 * gate is ignored.  A read returns the current owner, giving the standard
 * lock-then-read-back mutual exclusion two cores use to coordinate.
 *
 * FIDELITY NOTE — the model does not track which bus master issued the access,
 * so any master may free a gate (real silicon restricts the unlock to the
 * owner) and the RSTGT reset-gate sequence is register-accurate but not gated
 * by the secure-master check.  Flagged, not faked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/imxrt1180_sema42.h"
#include "migration/vmstate.h"

static uint64_t sema42_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180Sema42State *s = opaque;
    return off < IMXRT1180_SEMA42_NGATE ? s->gate[off] : 0;
}
static void sema42_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IMXRT1180Sema42State *s = opaque;
    if (off >= IMXRT1180_SEMA42_NGATE) {
        return;                         /* RSTGT / notify: register-accurate no-op */
    }
    uint8_t val = v & 0xF;
    if (val == 0) {
        s->gate[off] = 0;               /* unlock */
    } else if (s->gate[off] == 0) {
        s->gate[off] = val;             /* lock to this domain (first wins) */
    }                                   /* else already owned -> ignored */
}
static const MemoryRegionOps sema42_ops = {
    .read = sema42_read, .write = sema42_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 2,
    .impl.min_access_size = 1, .impl.max_access_size = 1,
};
static void sema42_reset(DeviceState *dev)
{
    IMXRT1180Sema42State *s = IMXRT1180_SEMA42(dev);
    memset(s->gate, 0, sizeof(s->gate));
}
static void sema42_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180Sema42State *s = IMXRT1180_SEMA42(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &sema42_ops, s, TYPE_IMXRT1180_SEMA42, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}
static const VMStateDescription vmstate_sema42 = {
    .name = TYPE_IMXRT1180_SEMA42, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(gate, IMXRT1180Sema42State, IMXRT1180_SEMA42_NGATE),
        VMSTATE_END_OF_LIST() } };
static void sema42_class_init(ObjectClass *k, const void *d)
{
    DeviceClass *dc = DEVICE_CLASS(k);
    dc->realize = sema42_realize; device_class_set_legacy_reset(dc, sema42_reset); dc->vmsd = &vmstate_sema42;
}
static const TypeInfo sema42_types[] = {{ .name = TYPE_IMXRT1180_SEMA42, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180Sema42State), .class_init = sema42_class_init }};
DEFINE_TYPES(sema42_types)
