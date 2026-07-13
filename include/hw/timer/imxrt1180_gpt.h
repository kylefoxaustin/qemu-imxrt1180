/*
 * NXP i.MX RT1180 GPT — General Purpose Timer (32-bit up-counter, OC1).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_IMXRT1180_GPT_H
#define HW_TIMER_IMXRT1180_GPT_H
#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"
#include "hw/misc/imxrt1180_ccm.h"
#define TYPE_IMXRT1180_GPT "imxrt1180-gpt"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180GPTState, IMXRT1180_GPT)
struct IMXRT1180GPTState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    uint32_t cr, pr, sr, ir, ocr1;
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
