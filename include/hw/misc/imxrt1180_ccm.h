/*
 * NXP i.MX RT1180 CCM — Clock Controller Module (clock roots, gates, observe).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_CCM_H
#define HW_MISC_IMXRT1180_CCM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_CCM "imxrt1180-ccm"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180CCMState, IMXRT1180_CCM)

#define IMXRT1180_CCM_SIZE 0x10000

struct IMXRT1180CCMState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[IMXRT1180_CCM_SIZE / 4];
};

#endif /* HW_MISC_IMXRT1180_CCM_H */
