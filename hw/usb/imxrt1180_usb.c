/*
 * NXP i.MX RT1180 USB 2.0 OTG (ChipIdea USB-HS) — device-mode controller model.
 *
 * The USB stack (USB_DeviceEhciInit / USB_DeviceRun) resets the controller,
 * reads its device capabilities, sets device mode, programs the endpoint queue
 * head list, and starts the controller — then waits to be enumerated by a USB
 * host.  This model is register-accurate for that bring-up:
 *
 *   - CAPLENGTH/HCIVERSION, DCIVERSION and DCCPARAMS report real capabilities
 *     (device+host capable, 8 device endpoints), so USB_DeviceInit no longer
 *     fails on "0 endpoints".
 *   - USBCMD.RST is self-clearing (reset reported complete).
 *   - USBSTS / ENDPTSETUPSTAT / ENDPTCOMPLETE are write-1-to-clear.
 *   - ENDPTPRIME / ENDPTFLUSH read back idle (no transfer engine to stay busy).
 *   - all other registers are plain register-backed storage.
 *
 * FIDELITY NOTE — this machine has no USB host attached, and this model does not
 * fabricate one.  It completes controller init/run so firmware progresses, but
 * it never asserts a USB reset / port-change, so the device stays un-enumerated
 * (the honest outcome for a headless target).  Modelling actual enumeration
 * would require bridging to QEMU's USB host framework and a device backend;
 * that is flagged future work, not silently faked.  No data transfers occur, so
 * no endpoint completion or interrupt is ever raised.
 *
 * Register offsets verified against the ChipIdea USB-HS IP (QEMU hw/usb/
 * chipidea.c: capsbase 0x100, opregbase 0x140; DCIVERSION 0x120, DCCPARAMS
 * 0x124) and the MIMXRT1189 USB_OTG1/2 memory map.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/usb/imxrt1180_usb.h"
#include "migration/vmstate.h"

/* Capability registers (base 0x100). */
#define USB_CAPLENGTH_HCIVERSION 0x100   /* CAPLENGTH[7:0] | HCIVERSION[31:16] */
#define USB_DCIVERSION           0x120
#define USB_DCCPARAMS            0x124

/* Operational registers (base 0x140). */
#define USB_USBCMD               0x140
#define USB_USBSTS               0x144
#define USB_ENDPTSETUPSTAT       0x1AC
#define USB_ENDPTPRIME           0x1B0
#define USB_ENDPTFLUSH           0x1B4
#define USB_ENDPTCOMPLETE        0x1BC

/* USBCMD bits. */
#define USBCMD_RS                0x00000001u   /* Run/Stop        */
#define USBCMD_RST               0x00000002u   /* controller reset (self-clear) */

/* DCCPARAMS bits: DEN[4:0]=device endpoints, DC=device-capable, HC=host-capable */
#define DCCPARAMS_DEN(n)         ((n) & 0x1Fu)
#define DCCPARAMS_DC             0x00000080u
#define DCCPARAMS_HC             0x00000100u

/* CAPLENGTH = opregbase - capsbase = 0x140 - 0x100 = 0x40; HCIVERSION = 0x0100 */
#define USB_CAPLENGTH            0x40u
#define USB_HCIVERSION           0x0100u

static uint64_t imxrt1180_usb_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180USBState *s = IMXRT1180_USB(opaque);

    if (offset + 4 > IMXRT1180_USB_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    switch (offset) {
    case USB_CAPLENGTH_HCIVERSION:
        return ((uint32_t)USB_HCIVERSION << 16) | USB_CAPLENGTH;
    case USB_DCIVERSION:
        return 0x00000001;
    case USB_DCCPARAMS:
        /* Device + host capable, 8 device endpoints (i.MX USB-HS). */
        return DCCPARAMS_HC | DCCPARAMS_DC | DCCPARAMS_DEN(8);
    case USB_USBCMD:
        /* RST is self-clearing: report the reset already complete. */
        return s->regs[offset / 4] & ~USBCMD_RST;
    case USB_ENDPTPRIME:
    case USB_ENDPTFLUSH:
        /* No transfer engine to stay busy: prime/flush complete instantly. */
        return 0;
    default:
        return s->regs[offset / 4];
    }
}

static void imxrt1180_usb_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMXRT1180USBState *s = IMXRT1180_USB(opaque);

    if (offset + 4 > IMXRT1180_USB_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    switch (offset) {
    case USB_USBSTS:
    case USB_ENDPTSETUPSTAT:
    case USB_ENDPTCOMPLETE:
        s->regs[offset / 4] &= ~(uint32_t)value;   /* W1C */
        return;
    case USB_USBCMD:
        s->regs[offset / 4] = value;
        if ((value & USBCMD_RS) && !s->running_logged) {
            s->running_logged = true;
            qemu_log_mask(LOG_UNIMP,
                "%s: controller started (RS=1) but no USB host is attached to "
                "this machine -- the device will not enumerate (not faked)\n",
                __func__);
        }
        return;
    default:
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps imxrt1180_usb_ops = {
    .read = imxrt1180_usb_read,
    .write = imxrt1180_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_usb_reset(DeviceState *dev)
{
    IMXRT1180USBState *s = IMXRT1180_USB(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->running_logged = false;
}

static void imxrt1180_usb_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180USBState *s = IMXRT1180_USB(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_usb_ops, s,
                          TYPE_IMXRT1180_USB, IMXRT1180_USB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imxrt1180_usb = {
    .name = TYPE_IMXRT1180_USB,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(running_logged, IMXRT1180USBState),
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180USBState, IMXRT1180_USB_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_usb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_usb_realize;
    device_class_set_legacy_reset(dc, imxrt1180_usb_reset);
    dc->vmsd = &vmstate_imxrt1180_usb;
}

static const TypeInfo imxrt1180_usb_types[] = {
    {
        .name          = TYPE_IMXRT1180_USB,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180USBState),
        .class_init    = imxrt1180_usb_class_init,
    },
};

DEFINE_TYPES(imxrt1180_usb_types)
