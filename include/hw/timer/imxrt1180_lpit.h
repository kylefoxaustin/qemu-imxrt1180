/*
 * NXP i.MX RT1180 LPIT — Low-Power Periodic Interrupt Timer (4 channels).
 *
 * Each channel is a 32-bit periodic down-counter: it loads TVAL, counts down to
 * zero at the peripheral clock, sets MSR.TIFn (raising the shared IRQ if
 * MIER.TIEn) and reloads.  Backed by a QEMU ptimer per channel.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_IMXRT1180_LPIT_H
#define HW_TIMER_IMXRT1180_LPIT_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_LPIT "imxrt1180-lpit"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180LPITState, IMXRT1180_LPIT)

#define IMXRT1180_LPIT_NCHAN 4

typedef struct IMXRT1180LPITChan {
    IMXRT1180LPITState *s;
    unsigned ch;
} IMXRT1180LPITChan;

struct IMXRT1180LPITState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer[IMXRT1180_LPIT_NCHAN];
    IMXRT1180LPITChan chan[IMXRT1180_LPIT_NCHAN];

    uint32_t mcr;                 /* Module Control (M_CEN) */
    uint32_t msr;                 /* Module Status (TIFn, W1C) */
    uint32_t mier;                /* Module Interrupt Enable (TIEn) */
    uint32_t tval[IMXRT1180_LPIT_NCHAN];
    uint32_t tctrl[IMXRT1180_LPIT_NCHAN];
    uint32_t freq;                /* peripheral clock (Hz) */
};

#endif /* HW_TIMER_IMXRT1180_LPIT_H */
