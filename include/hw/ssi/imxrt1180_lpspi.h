/*
 * NXP i.MX RT1180 LPSPI — Low-Power SPI, controller (master) mode.
 *
 * Full-duplex command-FIFO master: firmware sets TCR (frame size, PCS, CONT)
 * and writes words to TDR; each frame is shifted out to the selected SPI slave
 * over a QEMU SSIBus (MSB-first, byte at a time) and the received word lands in
 * the rx FIFO (RDR).  PCS drives per-chip-select lines like the i.MX ECSPI.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_SSI_IMXRT1180_LPSPI_H
#define HW_SSI_IMXRT1180_LPSPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_LPSPI "imxrt1180-lpspi"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180LPSPIState, IMXRT1180_LPSPI)

#define IMXRT1180_LPSPI_NUMCS 4
#define IMXRT1180_LPSPI_FIFO  16

struct IMXRT1180LPSPIState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq dma_tx_req;           /* TX -> eDMA hardware request line */
    qemu_irq dma_rx_req;           /* RX FIFO -> eDMA hardware request line */
    qemu_irq cs_lines[IMXRT1180_LPSPI_NUMCS];
    SSIBus *bus;

    uint32_t cr;
    uint32_t sr_sticky;            /* W1C flags (WCF/FCF/TCF/TEF/REF/DMF) */
    uint32_t ier;
    uint32_t der;
    uint32_t cfgr[2];              /* CFGR0/1 */
    uint32_t dmr[2];               /* DMR0/1 */
    uint32_t ccr[2];               /* CCR/CCR1 */
    uint32_t fcr;
    uint32_t tcr;                  /* Transmit Command */
    int      cs_active;            /* currently-asserted PCS, or -1 */

    uint32_t rx_fifo[IMXRT1180_LPSPI_FIFO];
    uint8_t  rx_head;
    uint8_t  rx_count;
};

#endif /* HW_SSI_IMXRT1180_LPSPI_H */
