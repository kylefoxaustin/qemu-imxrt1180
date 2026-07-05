/*
 * NXP i.MX RT1180 XBAR1 — inter-peripheral signal crossbar.
 *
 * Routes any of up to 256 input signals to any of 221 outputs: each output has
 * an 8-bit select field naming the input that drives it.  Used here to carry an
 * eFlexPWM trigger edge to the LPADC's hardware trigger inputs (PWM->ADC
 * synchronised sampling).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_XBAR_H
#define HW_MISC_IMXRT1180_XBAR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_XBAR "imxrt1180-xbar"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180XBARState, IMXRT1180_XBAR)

#define IMXRT1180_XBAR_SIZE   0x200
#define IMXRT1180_XBAR_NOUT   221      /* output signals            */
#define IMXRT1180_XBAR_NIN    256      /* input signals (8-bit sel) */

struct IMXRT1180XBARState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq out[IMXRT1180_XBAR_NOUT];
    bool     in_level[IMXRT1180_XBAR_NIN];
    uint16_t regs[IMXRT1180_XBAR_SIZE / 2];   /* SEL[] + CTRL[]      */
};

#endif /* HW_MISC_IMXRT1180_XBAR_H */
