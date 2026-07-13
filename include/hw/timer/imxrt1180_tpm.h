/*
 * NXP i.MX RT1180 TPM — Timer/PWM Module (16-bit up-counter, overflow).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_IMXRT1180_TPM_H
#define HW_TIMER_IMXRT1180_TPM_H
#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"
#include "hw/misc/imxrt1180_ccm.h"
#define TYPE_IMXRT1180_TPM "imxrt1180-tpm"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180TPMState, IMXRT1180_TPM)
struct IMXRT1180TPMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    uint32_t sc, mod, status;
    uint32_t regs[0x100 / 4];   /* backing for CONTROLS[] etc. (read-back match) */
    uint32_t clk;
    /* THE CLOCK IS NOT A CONSTANT.  It is CLOCK_ROOT[clk_root] in the CCM, read at
     * the point of use -- the guest rewrites the roots in BOARD_InitBootClocks()
     * and some examples re-mux again afterwards.  This block used to hold a
     * hardcoded default behind `if (!clk) clk = DEFAULT;`, which made the missing
     * wiring invisible and ran the timer at the wrong rate. */
    IMXRT1180CCMState *ccm;
    uint32_t clk_root;            /* kCLOCK_Root_* index */
};
#endif
