/*
 * NXP i.MX RT1180 RTWDOG (Watchdog) — register-accurate disable stub.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_RTWDOG_H
#define HW_MISC_IMXRT1180_RTWDOG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_RTWDOG "imxrt1180-rtwdog"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180RTWDOGState, IMXRT1180_RTWDOG)

struct IMXRT1180RTWDOGState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;

    uint32_t cs;
    uint32_t cnt;
    uint32_t toval;
    uint32_t win;
    uint8_t  unlock_step;   /* progress through the 0xC520,0xD928 unlock */
    bool     unlocked;
    bool     reconfigured;   /* a reconfiguration has actually completed (CS.RCS) */
};

#endif /* HW_MISC_IMXRT1180_RTWDOG_H */
