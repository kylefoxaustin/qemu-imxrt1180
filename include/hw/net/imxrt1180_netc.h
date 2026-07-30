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

/* Switch table capacities, programmed via the NTMP command BD ring. */
#define IMXRT1180_NETC_FDB_SIZE 64
#define IMXRT1180_NETC_VF_SIZE  32

/* Switch external (wire) ports: SW0 ports 0..3 are physical, port 4 is the CPU.
 * Each wire port can be backed by its own netdev, so the switch routes frames
 * between different physical wires (multi-physical-port).  Port 0 uses the
 * default `-nic`/`-netdev` (back-compat); ports 1..3 attach to netdevs named
 * "netc-port1".."netc-port3" when present, else stay unplugged. */
#define IMXRT1180_NETC_N_WIRE   4

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

/* One VLAN-filter entry (VID -> filtering-ID + VLAN port membership).  The
 * config element is stored raw (4 dwords) so a query round-trips exactly what
 * the driver programmed; port_membership = cfge[0][23:0], fid = cfge[1][11:0]. */
typedef struct IMXRT1180NETCVlanEntry {
    bool     valid;
    uint16_t vid;              /* keye.vid */
    uint32_t entry_id;         /* hardware-assigned entry handle */
    uint32_t cfge[4];          /* raw cfge: portMembership, fid/mlo/mfo, etaBitmap, baseETEID */
} IMXRT1180NETCVlanEntry;

/* Per-NIC opaque: lets a wire port's receive callback recover which switch port
 * (and which device) the inbound frame arrived on. */
typedef struct {
    IMXRT1180NETCState *s;
    int port;
} IMXRT1180NETCPort;

struct IMXRT1180NETCState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    AddressSpace *dma_as;      /* system memory, for BD/frame DMA */
    uint8_t *backing;          /* flat RW register store over the region */

    /* L2 Ethernet backend, one per wire port: a frame egressing switch port p
     * is sent on nic[p]; an inbound frame on nic[p] ingresses on switch port p.
     * port_ctx[p] is the per-NIC opaque so the receive callback knows its port. */
    NICState *nic[IMXRT1180_NETC_N_WIRE];
    NICConf conf[IMXRT1180_NETC_N_WIRE];
    IMXRT1180NETCPort port_ctx[IMXRT1180_NETC_N_WIRE];
    uint64_t rx_ring_full_drops;   /* frames the wire delivered with no free BD */

    /* EMDIO / behavioural PHY responder */
    uint32_t mdio_reg;         /* PHY register addressed by the last EMDIO_CTL */
    uint16_t phy_regs[32];     /* per-PHY-register scratch (writes echo back) */

    /* NETC switch (SW0) tables, programmed over the NTMP command BD ring
     * (CBDRPIR doorbell -> process BD -> advance CBDRCIR). */
    IMXRT1180NETCFdbEntry fdb[IMXRT1180_NETC_FDB_SIZE];
    uint32_t fdb_next_id;      /* next hardware-assigned FDB entry_id */
    IMXRT1180NETCVlanEntry vlan[IMXRT1180_NETC_VF_SIZE];
    uint32_t vlan_next_id;     /* next hardware-assigned VLAN-filter entry_id */

    /* PTP 1588 timer (TMR0): a nanosecond clock derived from the QEMU virtual
     * clock.  The count advances at rate = full_addend / nominal_addend (the
     * first addend written is the rate-1 reference), matching the driver's
     * digital-DDS frequency servo. */
    int64_t  ptp_t_base;         /* QEMU_CLOCK_VIRTUAL ns at the last re-latch   */
    uint64_t ptp_cnt_base;       /* raw counter value at the last re-latch       */
    uint64_t ptp_nominal_addend; /* full addend corresponding to rate 1.0        */
    uint32_t ptp_cur_hi_latch;   /* high word captured when CUR_TIME_L was read  */
    bool     ptp_enabled;        /* TMR_CTRL.TE                                  */
};

#endif /* HW_NET_IMXRT1180_NETC_H */
