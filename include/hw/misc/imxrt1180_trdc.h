/*
 * NXP i.MX RT1180 TRDC — Trusted Resource Domain Controller (config stub).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_TRDC_H
#define HW_MISC_IMXRT1180_TRDC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_TRDC "imxrt1180-trdc"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180TRDCState, IMXRT1180_TRDC)

#define IMXRT1180_TRDC_SIZE 0x1000

struct IMXRT1180TRDCState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[IMXRT1180_TRDC_SIZE / 4];
    uint32_t hwcfg0;   /* TRDC_HWCFG0: NDID/NMSTR/NMBC/NMRC counts (per instance) */
    uint32_t ncm_mask; /* bit N set => master N is a non-CPU master (DACFG.NCM=1) */
};

#endif /* HW_MISC_IMXRT1180_TRDC_H */
