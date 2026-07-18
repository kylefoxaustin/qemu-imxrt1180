/*
 * NXP i.MX RT1180 LPADC — 12/16-bit SAR ADC (command list + result FIFO).
 *
 * A trigger (software SWTRIG or a hardware trigger input, e.g. an eFlexPWM edge
 * routed through the XBAR) launches a command chain; each command samples a
 * channel and pushes a tagged result into a result FIFO the CPU reads.  The
 * motor-control front-end that samples phase currents synchronously with PWM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_ADC_H
#define HW_MISC_IMXRT1180_ADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_ADC "imxrt1180-adc"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180ADCState, IMXRT1180_ADC)

#define IMXRT1180_ADC_SIZE      0x1000
#define IMXRT1180_ADC_NFIFO     2
#define IMXRT1180_ADC_NTRIG     8      /* trigger sources / HW-trigger inputs */
#define IMXRT1180_ADC_FIFO_DEPTH 16

typedef struct IMXRT1180ADCFifo {
    uint32_t data[IMXRT1180_ADC_FIFO_DEPTH];
    uint8_t  head;
    uint8_t  count;
} IMXRT1180ADCFifo;

struct IMXRT1180ADCState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq dma_req[IMXRT1180_ADC_NFIFO];   /* result-FIFO -> eDMA request lines */
    qemu_irq trig_in[IMXRT1180_ADC_NTRIG];   /* HW trigger inputs (from XBAR) */
    bool no_afe_logged;                      /* one-shot "no analog input" flag */

    uint32_t regs[IMXRT1180_ADC_SIZE / 4];
    IMXRT1180ADCFifo fifo[IMXRT1180_ADC_NFIFO];
    /*
     * Per-channel sample code driven by a plant, indexed [side][channel]: side 0
     * is the A-side mux, side 1 the B-side.  A DualSingleEndBothSide command
     * converts BOTH sides of a channel number at once (A -> FIFO0, B -> FIFO1),
     * which is how mc_pmsm reads Ia on A5 and Ib on B5 in a single command.
     */
    uint16_t channel_input[2][32];
};

#define IMXRT1180_ADC_SIDE_A 0
#define IMXRT1180_ADC_SIDE_B 1

/*
 * Set the sample code a channel's A-side (side 0) or B-side (side 1) mux returns
 * on its next conversion.  A virtual-motor plant calls this to inject phase-current
 * samples; unset (side,channel) pairs read back the neutral mid-scale placeholder.
 */
void imxrt1180_adc_set_channel_input(IMXRT1180ADCState *s, unsigned ch,
                                     unsigned side, uint16_t code);

#endif /* HW_MISC_IMXRT1180_ADC_H */
