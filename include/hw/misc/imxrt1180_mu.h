/*
 * NXP i.MX RT1180 MU — inter-core Messaging Unit (Cortex-M33 <-> Cortex-M7).
 *
 * One MU has two register "sides": MUA (0x4422_0000, the CM33) and MUB
 * (0x4423_0000, the CM7).  A write to MUA.TR[n] lands in MUB.RR[n] (and vice
 * versa); the general-purpose interrupt registers (GCR/GSR/GIER) implement a
 * cross-core doorbell.  This single device models both sides and cross-wires
 * them, raising each side's IRQ into the owning core's NVIC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_MU_H
#define HW_MISC_IMXRT1180_MU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_MU "imxrt1180-mu"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180MUState, IMXRT1180_MU)

#define IMXRT1180_MU_NCHAN 4   /* TR/RR/GIR count (from MU_PAR) */

/* Per-side MMIO port; opaque handed to the read/write ops so they know which
 * side (0 = MUA/CM33, 1 = MUB/CM7) issued the access. */
typedef struct IMXRT1180MUPort {
    IMXRT1180MUState *mu;
    unsigned side;
} IMXRT1180MUPort;

struct IMXRT1180MUState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem[2];
    IMXRT1180MUPort port[2];
    qemu_irq irq[2];              /* [0] -> CM33 NVIC, [1] -> CM7 NVIC */

    /* Message channels: chan[s][n] is the word side s transmitted (the other
     * side reads it from its RR[n]); full[s] bit n marks it pending. */
    uint32_t chan[2][IMXRT1180_MU_NCHAN];
    uint8_t  full[2];

    /* Per-side control/status backing. */
    uint32_t cr[2];              /* Control (MUR reset, MURIE) */
    uint32_t tcr[2];             /* Transmit interrupt enable (TIEn) */
    uint32_t rcr[2];             /* Receive interrupt enable (RIEn) */
    uint32_t gier[2];            /* GP interrupt enable (GIEn) */
    uint32_t gcr[2];             /* GP interrupt request (GIRn) */
    uint32_t gsr[2];             /* GP interrupt pending (GIPn) */
    uint32_t fcr[2];             /* Flag control (Fn) */
};

#endif /* HW_MISC_IMXRT1180_MU_H */
