/*
 * NXP i.MX RT1180 USBPHY — USB 2.0 high-speed PHY PLL readiness model.
 *
 * USB bring-up (SDK CLOCK_EnableUsbhs0PhyPllClock / the USB host+device stack's
 * PHY init) programs the PHY PLL and then spins:
 *
 *     while (0 == (USBPHY->PLL_SIC & USBPHY_PLL_SIC_PLL_LOCK)) { }
 *
 * waiting for the 480 MHz PHY PLL to lock.  This model is register-backed with
 * the i.MX "hardware macro" RW/SET/CLR/TOG bank convention (every register X has
 * SET @X+4, CLR @X+8, TOG @X+C), and reports PLL_SIC.PLL_LOCK set once firmware
 * has powered the PLL (PLL_POWER) — an instant, honest lock, mirroring the
 * ANADIG OSC/PLL readiness stubs.  It does NOT model USB data-line signalling,
 * UTMI, or charger detection (not needed to bring up the controller, and flagged
 * rather than faked).
 *
 * Register offsets/bits verified against the MIMXRT1189 USBPHY (fsl_clock.c
 * CLOCK_EnableUsbhs0PhyPllClock) and the shared i.MX USBPHY IP definition.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_usbphy.h"
#include "migration/vmstate.h"

#define USBPHY_PLL_SIC          0xA0    /* PHY PLL control/status              */

#define PLL_SIC_PLL_POWER       0x00001000u  /* bit 12: PLL powered up         */
#define PLL_SIC_PLL_EN_USB_CLKS 0x00000040u  /* bit  6: gate USB clocks on     */
#define PLL_SIC_PLL_LOCK        0x80000000u  /* bit 31: RO, PLL locked         */

static uint64_t imxrt1180_usbphy_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180USBPHYState *s = IMXRT1180_USBPHY(opaque);

    if (offset + 4 > IMXRT1180_USBPHY_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    /* SET/CLR/TOG aliases read back the underlying base register. */
    uint32_t base = offset & ~0xCu;
    uint32_t val  = s->regs[base / 4];

    if (base == USBPHY_PLL_SIC) {
        /*
         * The 480 MHz PHY PLL locks (near-)instantly once powered.  Report LOCK
         * whenever firmware has powered the PLL or enabled the USB clocks, so
         * the "wait for lock" spin terminates.
         */
        if (val & (PLL_SIC_PLL_POWER | PLL_SIC_PLL_EN_USB_CLKS)) {
            val |= PLL_SIC_PLL_LOCK;
        }
    }
    return val;
}

static void imxrt1180_usbphy_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IMXRT1180USBPHYState *s = IMXRT1180_USBPHY(opaque);

    if (offset + 4 > IMXRT1180_USBPHY_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    /*
     * i.MX register-bank convention: for base register X, X+4 = SET (OR),
     * X+8 = CLR (AND NOT), X+C = TOG (XOR); X itself is a plain write.
     */
    uint32_t base = offset & ~0xCu;
    uint32_t idx  = base / 4;
    switch (offset & 0xCu) {
    case 0x0: s->regs[idx]  = value;  break;
    case 0x4: s->regs[idx] |= value;  break;
    case 0x8: s->regs[idx] &= ~(uint32_t)value; break;
    case 0xC: s->regs[idx] ^= value;  break;
    }
}

static const MemoryRegionOps imxrt1180_usbphy_ops = {
    .read = imxrt1180_usbphy_read,
    .write = imxrt1180_usbphy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_usbphy_reset(DeviceState *dev)
{
    IMXRT1180USBPHYState *s = IMXRT1180_USBPHY(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imxrt1180_usbphy_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180USBPHYState *s = IMXRT1180_USBPHY(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_usbphy_ops, s,
                          TYPE_IMXRT1180_USBPHY, IMXRT1180_USBPHY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imxrt1180_usbphy = {
    .name = TYPE_IMXRT1180_USBPHY,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180USBPHYState,
                             IMXRT1180_USBPHY_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_usbphy_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_usbphy_realize;
    device_class_set_legacy_reset(dc, imxrt1180_usbphy_reset);
    dc->vmsd = &vmstate_imxrt1180_usbphy;
}

static const TypeInfo imxrt1180_usbphy_types[] = {
    {
        .name          = TYPE_IMXRT1180_USBPHY,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180USBPHYState),
        .class_init    = imxrt1180_usbphy_class_init,
    },
};

DEFINE_TYPES(imxrt1180_usbphy_types)
