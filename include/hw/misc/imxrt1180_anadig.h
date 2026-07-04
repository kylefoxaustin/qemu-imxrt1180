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
    /* Per-PFD-register "relock transient": the first read after a write to a
     * SYS_PLLn_PFD register reports the PFD(s) as momentarily NOT stable, so the
     * SDK's "wait for the stable bit to change" reconfigure loop terminates.
     * bit0 = SYS_PLL3_PFD (0x4030), bit1 = SYS_PLL2_PFD (0x4070). */
    uint8_t pfd_relock;
};

#endif /* HW_MISC_IMXRT1180_ANADIG_H */
