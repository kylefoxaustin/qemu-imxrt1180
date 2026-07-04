/*
 * NXP i.MX RT1180 ANADIG (analog clock/PLL/OSC control) — clock-ready model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_ANADIG_H
#define HW_MISC_IMXRT1180_ANADIG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_ANADIG "imxrt1180-anadig"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180AnadigState, IMXRT1180_ANADIG)

/* Window covers the OSC (0x43xx) + PLL (0x40xx) control/status registers. */
#define IMXRT1180_ANADIG_SIZE 0x8000

struct IMXRT1180AnadigState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[IMXRT1180_ANADIG_SIZE / 4];
};

#endif /* HW_MISC_IMXRT1180_ANADIG_H */
