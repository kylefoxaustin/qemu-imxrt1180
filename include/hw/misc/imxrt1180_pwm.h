/*
 * NXP i.MX RT1180 eFlexPWM — enhanced FlexPWM (motor-control PWM).
 *
 * One PWM module = 4 submodules (SM0..3), each an up-counter (INIT..VAL1) that
 * reloads periodically and drives a complementary PWMA/PWMB pair whose edges are
 * set by VAL2..VAL5.  Backed by a QEMU ptimer per submodule for the periodic
 * reload interrupt that clocks a field-oriented-control loop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_PWM_H
#define HW_MISC_IMXRT1180_PWM_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"
#include "hw/misc/imxrt1180_ccm.h"

#define TYPE_IMXRT1180_PWM "imxrt1180-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180PWMState, IMXRT1180_PWM)

#define IMXRT1180_PWM_NSM    4              /* submodules per module      */
#define IMXRT1180_PWM_SIZE   0x200          /* register window            */

typedef struct IMXRT1180PWMSub {
    IMXRT1180PWMState *pwm;
    unsigned idx;
} IMXRT1180PWMSub;

struct IMXRT1180PWMState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq_sm[IMXRT1180_PWM_NSM];     /* per-submodule compare/reload */
    qemu_irq irq_fault;                     /* fault / reload-error         */
    qemu_irq out_trig[IMXRT1180_PWM_NSM];   /* submodule output trigger (->XBAR) */
    qemu_irq dma_req[IMXRT1180_PWM_NSM];    /* per-submodule value-DMA request  */

    ptimer_state   *timer[IMXRT1180_PWM_NSM];
    IMXRT1180PWMSub sub[IMXRT1180_PWM_NSM];

    uint16_t regs[IMXRT1180_PWM_SIZE / 2];  /* full 16-bit register file    */
    /* Double-buffered (VALDE) registers: shadow written now, committed to
     * regs[] on MCTRL.LDOK (or immediately in LDMOD). INIT + VAL0..VAL5. */
    uint16_t buf_init[IMXRT1180_PWM_NSM];
    uint16_t buf_val[IMXRT1180_PWM_NSM][6];

    uint16_t duty[IMXRT1180_PWM_NSM];       /* computed PWMA duty, per-mille */
    uint32_t pwm_clk;                       /* submodule counter clock (Hz)  */
    /* THE CLOCK IS NOT A CONSTANT.  It is CLOCK_ROOT[clk_root] in the CCM, read at
     * the point of use -- the guest rewrites the roots in BOARD_InitBootClocks()
     * and some examples re-mux again afterwards.  This block used to hold a
     * hardcoded default behind `if (!clk) clk = DEFAULT;`, which made the missing
     * wiring invisible and ran the timer at the wrong rate. */
    IMXRT1180CCMState *ccm;
    uint32_t clk_root;            /* kCLOCK_Root_* index */
};

/* Accessors for a virtual-motor plant. */
uint16_t imxrt1180_pwm_duty(IMXRT1180PWMState *s, unsigned sm);   /* per-mille */
bool     imxrt1180_pwm_running(IMXRT1180PWMState *s, unsigned sm);

#endif /* HW_MISC_IMXRT1180_PWM_H */
