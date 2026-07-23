/*
 * NXP i.MX RT1180 XCACHE (platform cache controller) — control-plane model.
 * See the header for the rationale (completion, not caching).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/imxrt1180_xcache.h"
#include "migration/vmstate.h"

/* Per-instance register offsets (repeated at bank +0x000 and +0x800). */
#define XCACHE_CCR   0x0   /* Cache Control       */
#define XCACHE_CLCR  0x4   /* Cache Line Control  */
#define XCACHE_CSAR  0x8   /* Cache Search Addr   */
#define XCACHE_CCVR  0xC   /* Cache R/W Value     */

/* CCR transient command bits — all self-clear on completion. */
#define CCR_ENCACHE  0x00000001u
#define CCR_INVW0    0x01000000u
#define CCR_PUSHW0   0x02000000u
#define CCR_INVW1    0x04000000u
#define CCR_PUSHW1   0x08000000u
#define CCR_GO       0x80000000u
#define CCR_CMD_MASK (CCR_INVW0 | CCR_PUSHW0 | CCR_INVW1 | CCR_PUSHW1 | CCR_GO)

/* CSAR line-command bit. */
#define CSAR_LGO     0x00000001u

static uint64_t xcache_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180XcacheState *s = opaque;
    if (off + 4 > IMXRT1180_XCACHE_SIZE) {
        return 0;
    }
    return s->regs[off / 4];
}

static void xcache_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IMXRT1180XcacheState *s = opaque;
    uint32_t v = val;

    if (off + 4 > IMXRT1180_XCACHE_SIZE) {
        return;
    }

    /* Both instances share the same register layout; the bank offset (0x000 or
     * 0x800) is irrelevant to the per-register behaviour. */
    switch (off & 0x7FF) {
    case XCACHE_CCR:
        /* A maintenance command completes instantly (coherent memory, no data
         * cache): drop the transient command + GO bits so the driver's poll
         * reads "done" at once. Config bits (ENCACHE, ...) stick. */
        s->regs[off / 4] = v & ~CCR_CMD_MASK;
        break;
    case XCACHE_CSAR:
        /* Line command (LGO) likewise completes instantly. */
        s->regs[off / 4] = v & ~CSAR_LGO;
        break;
    default:
        s->regs[off / 4] = v;   /* CLCR, CCVR: plain storage */
        break;
    }
}

static const MemoryRegionOps xcache_ops = {
    .read = xcache_read,
    .write = xcache_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void xcache_reset(DeviceState *dev)
{
    IMXRT1180XcacheState *s = IMXRT1180_XCACHE(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static void xcache_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180XcacheState *s = IMXRT1180_XCACHE(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &xcache_ops, s,
                          TYPE_IMXRT1180_XCACHE, IMXRT1180_XCACHE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_xcache = {
    .name = TYPE_IMXRT1180_XCACHE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180XcacheState, IMXRT1180_XCACHE_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void xcache_class_init(ObjectClass *k, const void *d)
{
    DeviceClass *dc = DEVICE_CLASS(k);
    dc->realize = xcache_realize;
    device_class_set_legacy_reset(dc, xcache_reset);
    dc->vmsd = &vmstate_xcache;
}

static const TypeInfo xcache_types[] = {{
    .name = TYPE_IMXRT1180_XCACHE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180XcacheState),
    .class_init = xcache_class_init,
}};
DEFINE_TYPES(xcache_types)
