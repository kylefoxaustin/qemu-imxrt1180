/*
 * NXP i.MX RT1180 NETC — integrated PCIe Ethernet controller/switch.
 *
 * Models the ENETC endpoint path for the SDK netc_txrx_transfer example (internal
 * MAC loopback: IERB/PCI/capability register semantics, a behavioural EMDIO +
 * RTL8201 PHY responder, and a TX->RX buffer-descriptor loopback that fires the
 * MSI-X completion), plus the NETC switch (SW0) NTMP command-BD ring and its L2
 * forwarding database (FDB add/query/delete) that the fsl_netc_switch driver
 * programs its tables through.
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

/* Switch forwarding-database (FDB) capacity, programmed via the NTMP command BD. */
#define IMXRT1180_NETC_FDB_SIZE 64

/* One L2 forwarding-database entry (MAC + filtering-ID -> destination ports). */
typedef struct IMXRT1180NETCFdbEntry {
    bool     valid;
    bool     dynamic;          /* 0 = static, 1 = dynamic/learned (cfge.dynamic) */
    uint8_t  mac[6];           /* keye.macAddr */
    uint16_t fid;              /* keye.fid (filtering ID) */
    uint32_t port_bitmap;      /* cfge.portBitmap (forwarding destination ports) */
    uint32_t cfge_flags;       /* cfge 2nd dword: oETEID/ePort/iMirE/ctd/dynamic/... */
    uint32_t et_eid;           /* cfge.etEID */
    uint32_t entry_id;         /* hardware-assigned entry handle */
} IMXRT1180NETCFdbEntry;

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

    /* NETC switch (SW0) forwarding database, programmed over the NTMP command
     * BD ring (CBDRPIR doorbell -> process BD -> advance CBDRCIR). */
    IMXRT1180NETCFdbEntry fdb[IMXRT1180_NETC_FDB_SIZE];
    uint32_t fdb_next_id;      /* next hardware-assigned FDB entry_id */
};

#endif /* HW_NET_IMXRT1180_NETC_H */
