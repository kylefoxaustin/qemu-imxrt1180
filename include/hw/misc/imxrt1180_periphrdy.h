/*
 * NXP i.MX RT1180 generic peripheral readiness block.
 *
 * A register-backed model for peripherals whose data path is not (yet) modelled:
 * writes stick and read back, and an optional single "ready/idle" status bit is
 * forced set so a firmware readiness poll completes.  Used for the SINC, SPDIF,
 * PDM, SEMC, I3C and USBNC blocks — register-accurate config, no data transfer
 * (flagged, never faked).  Behavioural peripherals get their own dedicated model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_PERIPHRDY_H
#define HW_MISC_IMXRT1180_PERIPHRDY_H
#include "hw/core/sysbus.h"
#include "qom/object.h"
#define TYPE_IMXRT1180_PERIPHRDY "imxrt1180-periphrdy"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180PeriphRdyState, IMXRT1180_PERIPHRDY)
struct IMXRT1180PeriphRdyState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t *regs;
    uint32_t mmsize;      /* region size (bytes)                 */
    uint32_t rdy_off;     /* offset of the status reg (or 0xFFFFFFFF = none) */
    uint32_t rdy_mask;    /* bits forced set on that reg's read  */
};
#endif
