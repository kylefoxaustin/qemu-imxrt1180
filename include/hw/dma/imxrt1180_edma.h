/*
 * NXP i.MX RT1180 eDMA (enhanced DMA) — functional model.
 *
 * Channels carry a Transfer Control Descriptor (TCD).  TWO things request that a
 * channel be serviced, and THEY ARE THE SAME EVENT:
 *
 *  - SOFTWARE START -- writing TCD_CSR[START].
 *  - A PERIPHERAL HARDWARE REQUEST -- CH_MUX[SRC] selects the source, CH_CSR[ERQ]
 *    arms it.  This is how essentially every real transfer works
 *    (LPUART_TransferSendEDMA, SAI_TransferSendEDMA, ADC-to-memory): firmware
 *    arms a channel at the peripheral's data register and then NEVER TOUCHES THE
 *    DATA REGISTER AGAIN -- the peripheral's FIFO drives it.
 *
 * ★ EITHER ONE IS A *SERVICE REQUEST*, AND ONE SERVICE REQUEST MOVES EXACTLY
 *   ONE MINOR LOOP (NBYTES).  CITER decrements by one.  A channel with CITER=N
 *   needs N REQUESTS, of either kind, to complete its major loop -- and only the
 *   LAST of them applies SLAST/DLAST, reloads CITER from BITER, sets CH_CSR[DONE]
 *   and raises INTMAJOR.  TCD_CSR[DREQ] then auto-clears ERQ.
 *
 *   ⚠ RETRACTION (2026-07-12).  THIS COMMENT PREVIOUSLY SAID, AND THE CODE DID:
 *   "SOFTWARE START. Writing TCD_CSR[START] runs the WHOLE MAJOR LOOP."  **That
 *   was false.**  I modelled the hardware-request path correctly as one minor loop
 *   per request, and left the software path draining the entire major loop -- so
 *   the model held two contradictory semantics for the same event and I did not
 *   notice, because I wrote the sentence that made them look like different
 *   things.  Three independent sources say otherwise:
 *
 *     RM 5.5.5.2 "Multiple requests" (CITER=BITER=2): "The channel retires, which
 *        concludes ONE ITERATION OF THE MAJOR LOOP" ... then "9. SECOND hardware
 *        request..." -- two requests for two iterations.
 *     RM 5.4: "software and the TCDn_CSR[START] field FOLLOWS THE SAME BASIC FLOW
 *        AS PERIPHERAL REQUESTS."
 *     fsl_edma.h EDMA_TriggerChannelStart(): "This function starts a MINOR LOOP
 *        transfer."
 *
 *   And the SDK's own driver says it by CALLING IT TWICE: the stock
 *   edma4/channel_link example issues EDMA_TriggerChannelStart() back to back on
 *   the same channel, because its CITER is 2.
 *
 *   WHY NO TEST CAUGHT IT: every eDMA test we had used **CITER = 1**, where one
 *   minor loop IS the whole major loop and the two models are INDISTINGUISHABLE.
 *   The stock edma4/memory_to_memory example is the same shape -- it deliberately
 *   sets minorLoopBytes = the ENTIRE buffer, so CITER=1 -- which is exactly why it
 *   passed against a wrong model.  (mcxn947qemu found this the expensive way: their
 *   whole-major-loop START ran the transfer a SECOND time on the example's second
 *   START, walked off the end of the linked channels' buffers, corrupted guest
 *   memory and HARD-FAULTED the CPU -- while every instrument they had reported
 *   the DMA was fine.)
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

/* A channel may link to itself (RM 5.5.3). Bound the chain; do not recurse forever. */
#define IMXRT1180_EDMA_MAX_LINK_DEPTH 64

typedef struct IMXRT1180EDMAChan {
    uint32_t csr, es, intr, sbr, pri, mux;
    uint32_t mattr;      /* CH_MATTR @0x18 -- DMA4 only, reserved on DMA3 */
    uint32_t tcd_saddr, tcd_slast, tcd_daddr, tcd_dlast;
    uint32_t tcd_nbytes;
    uint16_t tcd_soff, tcd_attr, tcd_doff, tcd_citer, tcd_csr, tcd_biter;

    /*
     * Deferred software service requests (TCD_CSR[START]).  A START does NOT run
     * the minor loop inline: it bumps this counter and schedules the service BH,
     * so the transfer completes AFTER the triggering MMIO write returns -- exactly
     * as a real in-flight eDMA transfer does.  This is load-bearing: the SDK's
     * InitCM7DMA issues START, then W1C-clears CH_CSR[DONE] to drop a *stale* flag,
     * then polls DONE for THIS transfer.  A synchronous START sets DONE before that
     * clear, the clear wipes the fresh flag, and the poll hangs forever.  A COUNTER
     * (not a bool) because a channel with CITER=N is driven by N back-to-back STARTs
     * that may all land before the BH runs -- each must still buy exactly one loop.
     */
    uint32_t sw_start_pending;
} IMXRT1180EDMAChan;

struct IMXRT1180EDMAState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq[IMXRT1180_EDMA_MAX_CHANNELS];

    /* Peripheral hardware-request lines (one per CMSIS request-mux source). */
    uint32_t     ch_stride;   /* DMA3 = 0x10000, DMA4 = 0x8000 (see the .c) */
    unsigned     link_depth;  /* channel-link recursion guard (a channel may link
                               * to ITSELF -- RM 5.5.3) */
    bool         req[IMXRT1180_EDMA_NUM_REQ];
    QEMUBH      *bh;          /* services HARDWARE requests OUTSIDE MMIO dispatch */
    QEMUTimer   *sw_timer;    /* completes SOFTWARE (TCD_CSR[START]) transfers after
                              * a modelled duration, so they are in-flight across a
                              * driver's clear-then-poll of CH_CSR[DONE] (see the .c) */

    uint32_t num_channels;        /* active channels (DMA3=32, DMA4=64) */
    uint32_t mp_csr;
    uint32_t mp_es;
    uint32_t ch_grpri[IMXRT1180_EDMA_MAX_CHANNELS];
    IMXRT1180EDMAChan ch[IMXRT1180_EDMA_MAX_CHANNELS];
};

#endif /* HW_DMA_IMXRT1180_EDMA_H */
