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

/*
 * The TRDC aperture spans the control block plus the MBC (memory block checker)
 * and MRC (memory region checker) managed-region descriptors, which sit well
 * above the base (e.g. MRC region descriptors at +0x14000).  fsl_trdc writes a
 * descriptor then asserts the readback matches, so the whole aperture must be
 * register-backed -- size it to cover it.
 */
#define IMXRT1180_TRDC_SIZE 0x20000

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
