/*
 * NXP i.MX RT1180 USB 2.0 OTG (ChipIdea USB-HS) — device-mode controller model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_USB_IMXRT1180_USB_H
#define HW_USB_IMXRT1180_USB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_USB "imxrt1180-usb"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180USBState, IMXRT1180_USB)

/* USB core register block (0x000..0x1FF); USBNC wrapper begins at +0x200. */
#define IMXRT1180_USB_SIZE 0x200

struct IMXRT1180USBState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    bool         running_logged;   /* one-shot "no host attached" flag */
    uint32_t regs[IMXRT1180_USB_SIZE / 4];
};

#endif /* HW_USB_IMXRT1180_USB_H */
