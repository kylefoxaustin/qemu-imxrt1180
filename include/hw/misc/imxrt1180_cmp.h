/* NXP i.MX RT1180 CMP — analog comparator (register-accurate, no analog). SPDX: GPL-2.0-or-later */
#ifndef HW_MISC_IMXRT1180_CMP_H
#define HW_MISC_IMXRT1180_CMP_H
#include "hw/core/sysbus.h"
#include "qom/object.h"
#define TYPE_IMXRT1180_CMP "imxrt1180-cmp"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180CmpState, IMXRT1180_CMP)
struct IMXRT1180CmpState { SysBusDevice parent_obj; MemoryRegion iomem; qemu_irq irq; uint32_t regs[0x40/4]; };
#endif
