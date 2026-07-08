/* NXP i.MX RT1180 SEMA42 — hardware semaphores (gate array). SPDX: GPL-2.0-or-later */
#ifndef HW_MISC_IMXRT1180_SEMA42_H
#define HW_MISC_IMXRT1180_SEMA42_H
#include "hw/core/sysbus.h"
#include "qom/object.h"
#define TYPE_IMXRT1180_SEMA42 "imxrt1180-sema42"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180Sema42State, IMXRT1180_SEMA42)
#define IMXRT1180_SEMA42_NGATE 16
struct IMXRT1180Sema42State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t gate[IMXRT1180_SEMA42_NGATE];
};
#endif
