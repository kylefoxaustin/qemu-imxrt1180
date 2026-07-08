/*
 * NXP i.MX RT1180 LPTMR — Low-Power Timer (single 16-bit periodic counter).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_IMXRT1180_LPTMR_H
#define HW_TIMER_IMXRT1180_LPTMR_H
#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"
#define TYPE_IMXRT1180_LPTMR "imxrt1180-lptmr"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180LPTMRState, IMXRT1180_LPTMR)
struct IMXRT1180LPTMRState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    uint32_t csr, psr, cmr;   /* CNR is the live ptimer count */
    uint32_t clk;
};
#endif
