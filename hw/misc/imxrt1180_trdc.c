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
#include "hw/core/qdev-properties.h"
#include "hw/misc/imxrt1180_trdc.h"
#include "migration/vmstate.h"

#define TRDC_HWCFG0   0xF0
/* DACFG[] — Domain Assignment Config, uint8_t array @0x100 (one byte/master):
 * NMDAR[3:0] = #MDA registers, NCM[7] = 1 for a non-CPU (DMA/peripheral)
 * master.  fsl_trdc's Set{,Non}ProcessorDomainAssignment ASSERT on NCM. */
#define TRDC_DACFG_BASE   0x100
#define TRDC_DACFG_NCM    0x80
#define TRDC_DACFG_NMDAR  0x01   /* nominal: 1 master-domain-assignment register */

static uint64_t imxrt1180_trdc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180TRDCState *s = IMXRT1180_TRDC(opaque);

    if (offset + 4 > IMXRT1180_TRDC_SIZE) {
        return 0;
    }
    if (offset == TRDC_HWCFG0) {
        return s->hwcfg0;
    }
    /*
     * Synthesise the DACFG byte array (masters 0..7) from ncm_mask.  QEMU passes
     * the exact (possibly sub-word) offset for byte/halfword accesses and takes
     * the low bytes of what we return, so build the value starting at whatever
     * master `offset` lands on — the firmware reads DACFG[master] as a byte.
     */
    if (offset >= TRDC_DACFG_BASE && offset < TRDC_DACFG_BASE + 8) {
        uint32_t start_m = offset - TRDC_DACFG_BASE;
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            uint32_t m = start_m + b;
            uint8_t d = TRDC_DACFG_NMDAR;
            if (m < 8 && (s->ncm_mask & (1u << m))) {
                d |= TRDC_DACFG_NCM;
            }
            w |= (uint32_t)d << (b * 8);
        }
        return w;
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
    /* Accept byte/halfword access (QEMU does the sub-word extract/merge over the
     * word handler): secure firmware byte-pokes TRDC, and an over-strict window
     * would bus-fault it. */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_trdc_reset(DeviceState *dev)
{
    IMXRT1180TRDCState *s = IMXRT1180_TRDC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* Offsets from PERI_TRDC.h, values from the RM's cold-POR column. */
    s->regs[0x000 / 4] = 0x00000010;    /* TRDC_CR        */
    s->regs[0x1C0 / 4] = 0x00000008;    /* TRDC_IDAU_CR   */
    s->regs[0x1E4 / 4] = 0x01000000;    /* TRDC_FLW_PBASE */
}

/* NDID=16 domains, NMSTR=16 masters, NMBC=2, NMRC=2 blocks.  Counts must cover
 * the domain/master indices the SoC's TRDC-setup uses (domainId up to 0xB); the
 * MBC/MRC block config lives past this 4 KiB window and falls through to the
 * permissive catch-all, so this stub still enforces nothing (flagged). */
#define TRDC_HWCFG0_DEFAULT \
    ((16u) | (16u << 8) | (2u << 16) | (2u << 24))

static const Property imxrt1180_trdc_props[] = {
    DEFINE_PROP_UINT32("hwcfg0", IMXRT1180TRDCState, hwcfg0, TRDC_HWCFG0_DEFAULT),
    DEFINE_PROP_UINT32("ncm-mask", IMXRT1180TRDCState, ncm_mask, 0),
};

static void imxrt1180_trdc_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180TRDCState *s = IMXRT1180_TRDC(dev);

    if (s->hwcfg0 == 0) {
        s->hwcfg0 = TRDC_HWCFG0_DEFAULT;
    }
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
    device_class_set_props(dc, imxrt1180_trdc_props);
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
