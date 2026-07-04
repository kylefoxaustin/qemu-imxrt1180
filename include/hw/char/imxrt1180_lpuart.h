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

#define TYPE_IMXRT1180_LPUART "imxrt1180-lpuart"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180LPUARTState, IMXRT1180_LPUART)

struct IMXRT1180LPUARTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
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

    uint8_t  rx_byte;
    bool     rx_full;
};

#endif /* HW_CHAR_IMXRT1180_LPUART_H */
