/*
 * NXP i.MX RT1180 LPUART
 *
 * Standard NXP LPUART (the same IP as i.MX 93/95 and the MCXN LP_FLEXCOMM's
 * LPUART function) — modelled standalone here, since on the RT1180 the LPUART
 * is a plain peripheral, not wrapped in an LP_FLEXCOMM function selector.
 * Register offsets and bit positions VERIFIED against the MIMXRT1189 CMSIS
 * (PERI_LPUART.h): STAT.TDRE=23/TC=22/RDRF=21, CTRL.TE=19/RE=18/TIE=23/RIE=21.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_CHAR_IMXRT1180_LPUART_H
#define HW_CHAR_IMXRT1180_LPUART_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

/* PARAM/FIFO advertise 16, and FSL_FEATURE_LPUART_FIFO_SIZEn(x) == 16. */
#define IMXRT1180_LPUART_RXFIFO 16

#define TYPE_IMXRT1180_LPUART "imxrt1180-lpuart"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180LPUARTState, IMXRT1180_LPUART)

struct IMXRT1180LPUARTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    /*
     * eDMA request lines. Driven from BAUD[TDMAE]/BAUD[RDMAE] (bits 23/21,
     * PERI_LPUART.h) AND the corresponding data-flag: TX asserts while TDRE
     * (always, in this model -- writes are synchronous), RX while RDRF.
     * They are LEVEL lines, not pulses: the eDMA re-samples them per minor loop.
     */
    qemu_irq     dma_tx_req;
    qemu_irq     dma_rx_req;
    CharFrontend chr;

    /* Register state (only what the console path needs is meaningful). */
    uint32_t global;
    uint32_t pincfg;
    uint32_t baud;
    uint32_t ctrl;
    uint32_t match;
    uint32_t modir;
    uint32_t fifo;
    uint32_t water;

    /* Extended/timeout registers the SDK driver touches during init. */
    uint32_t reir;       /* Receiver Extended Idle    @0x48 */
    uint32_t teir;       /* Transmitter Extended Idle @0x4C */
    uint32_t hdcr;       /* Half Duplex Control       @0x50 */
    uint32_t tocr;       /* Timeout Control           @0x58 */
    uint32_t tosr;       /* Timeout Status            @0x5C */
    uint32_t timeout[4]; /* Timeout 0..3              @0x60..0x6C */

    /*
     * A REAL 16-DEEP RX FIFO.
     *
     * This used to be a ONE-BYTE HOLDING REGISTER (`uint8_t rx_byte; bool rx_full;`)
     * while PARAM and FIFO BOTH ADVERTISED SIXTEEN, and the SDK is compiled against
     * FSL_FEATURE_LPUART_FIFO_SIZEn(x) == 16.  A CAPABILITY REGISTER IS A CONTRACT:
     * we promised sixteen and delivered one.
     *
     * It was invisible because RDRF is defined against RXWATER, and RXWATER RESETS TO
     * ZERO -- so a 1-deep receiver and a 16-deep one are BEHAVIOURALLY IDENTICAL until
     * somebody sets a watermark.  Nothing in this tree ever did.  THE PROMISE WAS
     * NEVER CALLED IN.  (mcxn947qemu found the identical bug in their console LPUART
     * the same evening.)
     */
    uint8_t  rx_fifo[IMXRT1180_LPUART_RXFIFO];
    uint8_t  rx_head;      /* next byte to hand the guest */
    uint8_t  rx_count;     /* bytes currently in the FIFO */
};

#endif /* HW_CHAR_IMXRT1180_LPUART_H */
