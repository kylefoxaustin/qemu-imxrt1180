/*
 * NXP i.MX RT1180 NETC — integrated PCIe Ethernet controller/switch.
 *
 * Models enough of the ENETC endpoint path for the SDK netc_txrx_transfer
 * example to run as an internal MAC loopback (no external network): the
 * IERB/PCI/capability register semantics its driver init requires, a behavioural
 * EMDIO + RTL8201 PHY responder (link-up), and a TX->RX buffer-descriptor
 * loopback that fires the MSI-X completion message.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_NET_IMXRT1180_NETC_H
#define HW_NET_IMXRT1180_NETC_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_NETC "imxrt1180-netc"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180NETCState, IMXRT1180_NETC)

/* Region base 0x6000_0000; span covers up to ENETC1_SI1 + its MSI-X table. */
#define IMXRT1180_NETC_BASE 0x60000000
#define IMXRT1180_NETC_SIZE 0x00C20000

struct IMXRT1180NETCState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    AddressSpace *dma_as;      /* system memory, for BD/frame DMA */
    uint8_t *backing;          /* flat RW register store over the region */

    /* L2 Ethernet backend: TX egresses here (unless the PHY is in local
     * loopback), and inbound frames are delivered into the RX ring. */
    NICState *nic;
    uint64_t rx_ring_full_drops;   /* frames the wire delivered with no free BD */
    NICConf conf;

    /* EMDIO / behavioural PHY responder */
    uint32_t mdio_reg;         /* PHY register addressed by the last EMDIO_CTL */
    uint16_t phy_regs[32];     /* per-PHY-register scratch (writes echo back) */
};

#endif /* HW_NET_IMXRT1180_NETC_H */
