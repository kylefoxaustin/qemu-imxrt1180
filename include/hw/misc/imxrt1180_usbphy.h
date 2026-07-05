/*
 * NXP i.MX RT1180 USBPHY — USB 2.0 high-speed PHY PLL readiness model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_USBPHY_H
#define HW_MISC_IMXRT1180_USBPHY_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_USBPHY "imxrt1180-usbphy"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180USBPHYState, IMXRT1180_USBPHY)

#define IMXRT1180_USBPHY_SIZE 0x1000

struct IMXRT1180USBPHYState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[IMXRT1180_USBPHY_SIZE / 4];
};

#endif /* HW_MISC_IMXRT1180_USBPHY_H */
