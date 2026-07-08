/*
 * NXP i.MX RT1180 TMR — QuadTimer (4-channel 16-bit timer/counter).
 *
 * Each of the four channels is a 16-bit up-counter that counts a prescaled
 * clock (CTRL.PCS) while enabled (CTRL.CM + ENBL), compares against COMP1, sets
 * SCTRL.TCF and raises the shared IRQ (if SCTRL.TCFIE), then reloads from LOAD.
 * Backed by a QEMU ptimer per channel.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_IMXRT1180_TMR_H
#define HW_TIMER_IMXRT1180_TMR_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_TMR "imxrt1180-tmr"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180TMRState, IMXRT1180_TMR)

#define IMXRT1180_TMR_NCHAN 4
#define IMXRT1180_TMR_SIZE  0x80          /* 4 channels x 0x20 */

typedef struct IMXRT1180TMRChan {
    IMXRT1180TMRState *s;
    unsigned ch;
} IMXRT1180TMRChan;

struct IMXRT1180TMRState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state    *timer[IMXRT1180_TMR_NCHAN];
    IMXRT1180TMRChan chan[IMXRT1180_TMR_NCHAN];
    uint16_t regs[IMXRT1180_TMR_SIZE / 2];
    uint32_t tmr_clk;                     /* primary count clock (Hz) */
};

#endif /* HW_TIMER_IMXRT1180_TMR_H */
