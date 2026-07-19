/*
 * NXP i.MX RT1180 EQDC — Enhanced Quadrature Decoder (position/speed encoder).
 *
 * Decodes an incremental quadrature encoder (PHASEA/PHASEB/INDEX) into a 32-bit
 * position counter (UPOS:LPOS), a revolution counter (REV) and a per-read
 * position difference (POSD) that a motor-control loop uses for speed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_EQDC_H
#define HW_MISC_IMXRT1180_EQDC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_EQDC "imxrt1180-eqdc"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180EQDCState, IMXRT1180_EQDC)

#define IMXRT1180_EQDC_SIZE 0x100

struct IMXRT1180EQDCState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    bool no_encoder_logged;      /* one-shot "no plant" flag */
    uint16_t regs[IMXRT1180_EQDC_SIZE / 2];
};

/* Drive the position/revolution counters from a virtual-motor plant. */
void imxrt1180_eqdc_set_position(IMXRT1180EQDCState *s, uint32_t pos,
                                 uint16_t rev);
/* Present the hardware speed measurement (position difference over its period). */
void imxrt1180_eqdc_set_speed(IMXRT1180EQDCState *s, int16_t posd,
                              uint16_t posdper, uint16_t lastedge);
/* FILT[PRSC] prescaler exponent: QD-timer clock = bus clock / 2^PRSC. */
unsigned imxrt1180_eqdc_filt_prsc(IMXRT1180EQDCState *s);

#endif /* HW_MISC_IMXRT1180_EQDC_H */
