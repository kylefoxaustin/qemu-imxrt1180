/*
 * NXP i.MX RT1180 FlexSPI — controller readiness model (register-accurate).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_FLEXSPI_H
#define HW_MISC_IMXRT1180_FLEXSPI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_FLEXSPI "imxrt1180-flexspi"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180FlexSPIState, IMXRT1180_FLEXSPI)

#define IMXRT1180_FLEXSPI_SIZE 0x1000

struct IMXRT1180FlexSPIState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[IMXRT1180_FLEXSPI_SIZE / 4];
};

#endif /* HW_MISC_IMXRT1180_FLEXSPI_H */
