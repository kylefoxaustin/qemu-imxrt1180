/*
 * NXP i.MX RT1180 TPM — Timer/PWM Module (16-bit up-counter, overflow).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_IMXRT1180_TPM_H
#define HW_TIMER_IMXRT1180_TPM_H
#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"
#define TYPE_IMXRT1180_TPM "imxrt1180-tpm"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180TPMState, IMXRT1180_TPM)
struct IMXRT1180TPMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    uint32_t sc, mod, status;
    uint32_t clk;
};
#endif
