/*
 * NXP MCX N eDMA (enhanced DMA) — functional model.
 *
 * 16 channels, each with a Transfer Control Descriptor (TCD).  Writing
 * TCD_CSR.START software-triggers the channel: the model performs the minor
 * loop (NBYTES, with SOFF/DOFF strides and SSIZE/DSIZE element width) CITER
 * times across the system address space, then applies SLAST/DLAST, sets
 * CH_CSR.DONE + CH_INT and, if TCD_CSR.INTMAJOR is set, raises the channel
 * IRQ.  Register layout from the MCXN947 CMSIS header (DMA_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DMA_IMXRT1180_EDMA_H
#define HW_DMA_IMXRT1180_EDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_EDMA "imxrt1180-edma"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180EDMAState, IMXRT1180_EDMA)

#define IMXRT1180_EDMA_MAX_CHANNELS 64

typedef struct IMXRT1180EDMAChan {
    uint32_t csr, es, intr, sbr, pri, mux;
    uint32_t tcd_saddr, tcd_slast, tcd_daddr, tcd_dlast;
    uint32_t tcd_nbytes;
    uint16_t tcd_soff, tcd_attr, tcd_doff, tcd_citer, tcd_csr, tcd_biter;
} IMXRT1180EDMAChan;

struct IMXRT1180EDMAState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq[IMXRT1180_EDMA_MAX_CHANNELS];

    uint32_t num_channels;        /* active channels (DMA3=32, DMA4=64) */
    uint32_t mp_csr;
    uint32_t mp_es;
    uint32_t ch_grpri[IMXRT1180_EDMA_MAX_CHANNELS];
    IMXRT1180EDMAChan ch[IMXRT1180_EDMA_MAX_CHANNELS];
};

#endif /* HW_DMA_IMXRT1180_EDMA_H */
