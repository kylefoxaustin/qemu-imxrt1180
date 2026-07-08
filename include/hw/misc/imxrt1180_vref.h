/* NXP i.MX RT1180 VREF — voltage reference (readiness). SPDX: GPL-2.0-or-later */
#ifndef HW_MISC_IMXRT1180_VREF_H
#define HW_MISC_IMXRT1180_VREF_H
#include "hw/core/sysbus.h"
#include "qom/object.h"
#define TYPE_IMXRT1180_VREF "imxrt1180-vref"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180VrefState, IMXRT1180_VREF)
struct IMXRT1180VrefState { SysBusDevice parent_obj; MemoryRegion iomem; uint32_t regs[0x40/4]; };
#endif
