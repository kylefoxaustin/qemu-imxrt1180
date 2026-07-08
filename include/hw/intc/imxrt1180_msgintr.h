/*
 * NXP i.MX RT1180 MSGINTR — Message-Signalled Interrupt router.
 *
 * A writer (e.g. the NETC ENETC via MSI-X) writes an interrupt *index* N to a
 * channel's MSIIR register; the router sets bit N in that channel's MSIR pending
 * register and asserts its NVIC line.  The ISR reads MSIR (which clears it).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_INTC_IMXRT1180_MSGINTR_H
#define HW_INTC_IMXRT1180_MSGINTR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_MSGINTR "imxrt1180-msgintr"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180MSGINTRState, IMXRT1180_MSGINTR)

#define IMXRT1180_MSGINTR_CHANNELS 3

struct IMXRT1180MSGINTRState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t pending[IMXRT1180_MSGINTR_CHANNELS];  /* MSIR bits per channel */
};

#endif /* HW_INTC_IMXRT1180_MSGINTR_H */
