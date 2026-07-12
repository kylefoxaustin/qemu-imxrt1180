/*
 * NXP i.MX RT1180 eDMA (enhanced DMA) — functional model.
 *
 * Channels carry a Transfer Control Descriptor (TCD).  TWO things can move a
 * byte, and for a long time this model only had the first:
 *
 *  - SOFTWARE START.  Writing TCD_CSR[START] runs the whole major loop (the
 *    minor loop, NBYTES with SOFF/DOFF strides and SSIZE/DSIZE widths, CITER
 *    times), applies SLAST/DLAST, sets CH_CSR[DONE] and raises INTMAJOR.
 *
 *  - A PERIPHERAL HARDWARE REQUEST, which is how essentially every real
 *    transfer works (SAI_TransferSendEDMA, LPUART_TransferSendEDMA, ADC-to-
 *    memory).  Firmware arms a channel at the peripheral's data register, points
 *    CH_MUX[SRC] at that peripheral's request source, sets CH_CSR[ERQ], and then
 *    NEVER TOUCHES THE DATA REGISTER AGAIN -- the peripheral's FIFO drives it.
 *    **ONE MINOR LOOP PER REQUEST**, CITER decrementing, until the major loop
 *    completes.  TCD_CSR[DREQ] then auto-clears ERQ.
 *
 * Peripherals drive one GPIO per CMSIS request-mux source (`dma-req`, numbered
 * per PERI_DMA4.h: LPUART1 Tx=16/Rx=17, LPUART2 Tx=18/Rx=19, LPSPI1 Tx=11/Rx=12,
 * ...).  A channel consumes the source its CH_MUX[SRC] selects.
 *
 * ⚠ REQUESTS ARE SERVICED FROM A BOTTOM HALF, and that is not a detail.
 * A peripheral raises its request from inside its OWN MMIO write handler (the
 * LPUART asserts when firmware sets BAUD[TDMAE]).  If the DMA then wrote straight
 * back into that peripheral, it would be a RE-ENTRANT MMIO access and QEMU's
 * guard SILENTLY DROPS IT -- the channel still walks its minor loops, still
 * decrements CITER, still sets DONE, still raises INTMAJOR, while every byte it
 * "moved" is thrown away.  A test asking "did the transfer complete?" PASSES.
 * Only checking the DATA catches it.  (mcxn947qemu, 2026-07-12, who hit exactly
 * this: eight words "transferred" into an empty FIFO.)  A bottom half is also the
 * correct model: real DMA is asynchronous.
 *
 * Register layout from the MIMXRT1189 CMSIS header; request sources from
 * PERI_DMA4.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DMA_IMXRT1180_EDMA_H
#define HW_DMA_IMXRT1180_EDMA_H

#include "hw/core/sysbus.h"
#include "qemu/main-loop.h"   /* QEMUBH */
#include "qom/object.h"

#define TYPE_IMXRT1180_EDMA "imxrt1180-edma"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180EDMAState, IMXRT1180_EDMA)

#define IMXRT1180_EDMA_MAX_CHANNELS 64

/* CH_MUX[SRC] is 7 bits; PERI_DMA4.h numbers sources 0..~130. */
/*
 * CH_MUX[SRC] is an EIGHT-bit field: PERI_DMA4.h DMA4_CH_MUX_SRC_MASK = 0xFF.
 * (A 7-bit mask here would silently alias every source >= 128 onto a different
 * VALID source -- a wrong count truncates data quietly, which is worse than a
 * wrong offset, because a wrong offset at least hangs.)
 *
 * The SDK enum tags the controller instance in a high bit --
 *   kDma3RequestMuxLPUART1Tx = 16|0x100 -- and DMA_CH_MUX_SOURCE()'s & 0xFF
 * strips it, so the value that reaches the register is 16. Each controller has
 * its own 0..255 source space.
 */
#define IMXRT1180_EDMA_NUM_REQ 256

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

    /* Peripheral hardware-request lines (one per CMSIS request-mux source). */
    bool         req[IMXRT1180_EDMA_NUM_REQ];
    QEMUBH      *bh;          /* services requests OUTSIDE MMIO dispatch */

    uint32_t num_channels;        /* active channels (DMA3=32, DMA4=64) */
    uint32_t mp_csr;
    uint32_t mp_es;
    uint32_t ch_grpri[IMXRT1180_EDMA_MAX_CHANNELS];
    IMXRT1180EDMAChan ch[IMXRT1180_EDMA_MAX_CHANNELS];
};

#endif /* HW_DMA_IMXRT1180_EDMA_H */
