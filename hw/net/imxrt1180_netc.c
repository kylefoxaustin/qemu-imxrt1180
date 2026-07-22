/*
 * NXP i.MX RT1180 NETC — integrated PCIe Ethernet controller (ENETC endpoint).
 *
 * The NETC is a PCIe-structured block (config headers + ENETC controllers +
 * Station Interfaces + switch + EMDIO + PTP) spanning 0x6000_0000..0x60C1_xxxx.
 * This models the ENETC endpoint path exercised by the SDK netc_txrx_transfer
 * example, which runs as an internal MAC loopback (PHY local-loopback in the
 * firmware) and self-checks that each received frame equals the transmitted one.
 *
 * Modelled behaviours (all register offsets/masks from the MIMXRT1189 DFP):
 *   - NETC_PRIV NETCRR/NETCSR: IERB lock/unlock + soft-reset self-clear.
 *   - PCI config headers: PCI_CFH_CMD RW; PCI_CFC_PCIE_DEV_CTL INIT_FLR
 *     write-1-self-clear (the driver spins on the FLR bit).
 *   - ENETC capability regs (ECAPR1/2) + SI capability (SIPCAPR1): report enough
 *     TX/RX BD rings and MSI-X vectors that the driver's resource checks pass.
 *   - EMDIO + a behavioural RTL8201 PHY: ID match, reset self-clear, link-up.
 *   - TX->RX BD loopback: writing the TX producer index DMAs the frame from the
 *     TX BD, copies it into the RX ring's buffer, writes the RX BD back ready,
 *     and emits the TX/RX MSI-X messages so the driver's completion path runs.
 *   - SWITCH (SW0) NTMP command-BD ring + L2 tables: the fsl_netc_switch driver
 *     programs the switch's tables over a command BD ring (CBDRPIR doorbell ->
 *     process BD -> advance CBDRCIR).  We run add / query / update / delete
 *     synchronously for the forwarding database (FDB, {MAC,FID}->portBitmap) and
 *     the VLAN filter table (VF, VID->{FID, port membership}).  A frame ingressing
 *     the switch has its SOURCE MAC learned into a dynamic FDB entry, and a CPU-
 *     injected frame is FORWARDED per the FDB: the egress decision does the
 *     FDB (intersect VLAN membership) lookup, floods an unknown-unicast/broadcast,
 *     and applies split-horizon, so it reaches the wire only when its destination
 *     resolves there.  The wire->CPU forwarding side and multi-physical-port
 *     routing are not yet modelled (unmodelled tables fault honestly via the BD's
 *     resp.error, never a silent ack).
 *   - PTP 1588 timer (TMR0): a nanosecond clock derived from the QEMU virtual
 *     clock, whose rate the driver tunes via the addend (digital DDS).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/net/imxrt1180_netc.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "exec/memattrs.h"
#include "qemu/bswap.h"
#include "net/net.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* ---- Region-relative block offsets (base 0x6000_0000) -------------------- */
#define NETC_PRIV_BASE     0x900000
#define R_NETCRR           (NETC_PRIV_BASE + 0x100)  /* SR=1, LOCK=2 */
#define R_NETCSR           (NETC_PRIV_BASE + 0x104)  /* reads 0 (STATE/ERROR clear) */
#define NETCRR_SR          0x1
#define NETCRR_LOCK        0x2

#define ENETC0_BASE_OFF    0xB10000
#define R_ECAPR0           (ENETC0_BASE_OFF + 0x0)
#define R_ECAPR1           (ENETC0_BASE_OFF + 0x4)   /* NUM_MSIX[22:12], NUM_VSI[27:24] */
#define R_ECAPR2           (ENETC0_BASE_OFF + 0x8)   /* NUM_TX_BDR[9:0], NUM_RX_BDR[25:16] */

#define ENETC0_SI0_OFF     0xB00000
#define R_SIPCAPR1         (ENETC0_SI0_OFF + 0x24)   /* NUM_MSIX[17:12] */

/* ENETC0 Ethernet MAC/link block (0x60B15000): PMn_COMMAND_CONFIG.SWR is a
 * self-clearing software reset the driver spins on. */
#define ENETC0_ETH_OFF     0xB15000
#define R_PM0_CMD_CFG      (ENETC0_ETH_OFF + 0x008)
#define R_PM1_CMD_CFG      (ENETC0_ETH_OFF + 0x408)
#define PM_CMD_CFG_SWR     0x4000000u

/* Capability values: 8 TX + 8 RX BD rings, plenty of MSI-X, 0 VSIs. */
#define ECAPR1_VAL   ((6u << 12))                    /* NUM_MSIX=6, NUM_VSI=0 */
#define ECAPR2_VAL   ((8u << 0) | (8u << 16))        /* NUM_TX_BDR=8, NUM_RX_BDR=8 */
#define SIPCAPR1_VAL ((6u << 12))                    /* NUM_MSIX=6 */

/* EMDIO controller (within EMDIO_BASE @0x60BA0000, struct offset 0x1C00). */
#define EMDIO_OFF          0xBA0000
#define R_EMDIO_CFG        (EMDIO_OFF + 0x1C00)       /* BSY1=bit31, BSY2=bit0 */
#define R_EMDIO_CTL        (EMDIO_OFF + 0x1C04)       /* READ=bit15, PORT[9:5], DEV[4:0] */
#define R_EMDIO_DATA       (EMDIO_OFF + 0x1C08)
#define EMDIO_CFG_BSY      0x80000001u
#define EMDIO_CTL_READ     0x8000u
#define EMDIO_CTL_DEV_MASK 0x1Fu

/* Behavioural RTL8201 PHY: devId = (ID1<<16)|ID2 must equal 0x001CC816. */
#define PHY_ID1_VAL        0x001Cu
#define PHY_ID2_VAL        0xC816u
#define PHY_BMCR_RESET     0x8000u
#define PHY_BMCR_LOOPBACK  0x4000u   /* BMCR bit14: PHY local loopback */
/* BMSR: link-up (0x4) + auto-neg-complete (0x20) + capability bits. */
#define PHY_BMSR_VAL       0x782Du

/* PCI config header INIT_FLR bit (PCI_CFC_PCIE_DEV_CTL @+0x48). */
#define PCI_DEVCTL_OFF     0x48
#define PCI_INIT_FLR       0x8000u

/*
 * BD ring registers (ENETC0 SI0, BDR[0]).  Both the TX and RX ring live in the
 * BDR[ring] element at SI+0x8000 (step 0x200): TX regs at +0x00.., RX at +0x100.
 * The example uses ring 0.  Writing TBPIR (the TX producer index) is the "go"
 * trigger that drives the internal TX->RX loopback.
 */
#define R_TBBAR0    (ENETC0_SI0_OFF + 0x8010)
#define R_TBBAR1    (ENETC0_SI0_OFF + 0x8014)
#define R_TBPIR     (ENETC0_SI0_OFF + 0x8018)   /* TX producer index (trigger) */
#define R_TBCIR     (ENETC0_SI0_OFF + 0x801C)   /* TX consumer index */
#define R_TBLENR    (ENETC0_SI0_OFF + 0x8020)   /* LENGTH field == #BDs */
#define R_RBMR      (ENETC0_SI0_OFF + 0x8100)   /* RX ring mode; EN = bit 31 */
#define RBMR_EN     (1u << 31)                  /* ENETC_SI_RBMR_EN_MASK */
#define R_RBBSR     (ENETC0_SI0_OFF + 0x8108)
#define R_RBCIR     (ENETC0_SI0_OFF + 0x810C)
#define R_RBBAR0    (ENETC0_SI0_OFF + 0x8110)
#define R_RBBAR1    (ENETC0_SI0_OFF + 0x8114)
#define R_RBPIR     (ENETC0_SI0_OFF + 0x8118)   /* RX producer index (HW-advanced) */
#define R_RBLENR    (ENETC0_SI0_OFF + 0x8120)
#define R_SIMSITRVR0 (ENETC0_SI0_OFF + 0xB00)   /* TX ring 0 -> MSI-X entry idx */
#define R_SIMSIRRVR0 (ENETC0_SI0_OFF + 0xB80)   /* RX ring 0 -> MSI-X entry idx */
#define BDR_LEN_MASK 0x1FFF8u

/* MSI-X table for ENETC0PSI0: 0x60BC0000 + 0x10000*(3+0) = region 0xBF0000. */
#define NETC_MSIX_TABLE 0xBF0000
#define MSIX_CTRL_MASK  0x1u                     /* vector control: mask bit */

/* TX BD (16B): addr@0, bufLen@8, frameLen@10; writeback dword[1] written=bit26.
 * RX BD (16B): standard.addr@0; writeback bufLen@8, error@14, byte15 bit6/7. */
#define TXBD_WB_WRITTEN 0x04000000u              /* dword[1] bit 26 */
#define RXBD_WB_READY   0x40u                    /* byte 15 bit 6 (isReady) */
#define RXBD_WB_FINAL   0x80u                    /* byte 15 bit 7 (isFinal) */
#define NETC_FRAME_MAX  2048

/* PCI config-header block bases (region-relative). */
static const hwaddr netc_pci_hdrs[] = {
    0x0000, 0x1000, 0x2000, 0x3000, 0x4000, 0xF8000, 0x100000,
};

static bool netc_is_pci_flr(hwaddr off)
{
    for (size_t i = 0; i < ARRAY_SIZE(netc_pci_hdrs); i++) {
        if (off == netc_pci_hdrs[i] + PCI_DEVCTL_OFF) {
            return true;
        }
    }
    return false;
}

/* ---- flat byte-backed register store ------------------------------------ */
static uint64_t netc_backing_read(IMXRT1180NETCState *s, hwaddr off, unsigned size)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        v |= (uint64_t)s->backing[off + i] << (8 * i);
    }
    return v;
}

static void netc_backing_write(IMXRT1180NETCState *s, hwaddr off, uint64_t val,
                               unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        s->backing[off + i] = (val >> (8 * i)) & 0xFF;
    }
}

/* Behavioural PHY register read (clause-22). */
static uint16_t netc_phy_read(IMXRT1180NETCState *s, uint32_t reg)
{
    switch (reg) {
    case 0:  return s->phy_regs[0] & ~PHY_BMCR_RESET; /* BMCR: reset self-clears */
    case 1:  return PHY_BMSR_VAL;                     /* BMSR: link up */
    case 2:  return PHY_ID1_VAL;
    case 3:  return PHY_ID2_VAL;
    default: return s->phy_regs[reg & 0x1F];
    }
}

static uint32_t netc_reg(IMXRT1180NETCState *s, hwaddr off)
{
    return (uint32_t)netc_backing_read(s, off, 4);
}

/* Emit the MSI-X message for a ring: look up the entry the driver assigned,
 * and if it is unmasked, write msgData to msgAddr (which lands in the MSGINTR
 * router and raises its NVIC line). */
static void netc_emit_msix(IMXRT1180NETCState *s, uint32_t entry_idx)
{
    hwaddr e = NETC_MSIX_TABLE + (hwaddr)entry_idx * 16;
    uint64_t msg_addr = (uint64_t)netc_reg(s, e) | ((uint64_t)netc_reg(s, e + 4) << 32);
    uint32_t msg_data = netc_reg(s, e + 8);
    uint32_t ctrl = netc_reg(s, e + 12);
    uint32_t le;

    if (ctrl & MSIX_CTRL_MASK) {
        return;                 /* vector masked */
    }
    le = cpu_to_le32(msg_data);
    dma_memory_write(s->dma_as, msg_addr, &le, 4, MEMTXATTRS_UNSPECIFIED);
}

/* Deliver one frame into the RX ring: copy it into the posted RX buffer, write
 * the RX BD back ready, advance the RX producer index, emit the RX MSI-X. */
static void netc_deliver_rx(IMXRT1180NETCState *s, const uint8_t *frame,
                            uint32_t len)
{
    uint64_t base = (uint64_t)netc_reg(s, R_RBBAR0) | ((uint64_t)netc_reg(s, R_RBBAR1) << 32);
    uint32_t rlen = netc_reg(s, R_RBLENR) & BDR_LEN_MASK;   /* #BDs */
    uint32_t pir = netc_reg(s, R_RBPIR) & 0xFFFF;
    uint32_t cir;
    hwaddr bd;
    uint8_t sbd[8], wb[16];
    uint64_t buf;

    if (rlen == 0) {
        return;
    }

    /*
     * IS THERE A FREE DESCRIPTOR?  THE MODEL NEVER ASKED, AND THAT WAS SILENT
     * MEMORY CORRUPTION.
     *
     * The RX ring is a producer/consumer ring.  HW produces at RBPIR; SW frees a
     * descriptor by re-posting standard.addr and then writing RBCIR ("Update the Rx
     * consumer index to free idle BD" -- fsl_netc_endpoint.c:1253).  R_RBCIR was
     * DEFINED IN THIS FILE AND NEVER READ, so there was no ring-full check at all.
     *
     * What that cost, measured: the 16-byte writeback below overwrites bytes 0..7 of
     * the descriptor -- which are standard.addr, the buffer pointer the driver posted.
     * So a descriptor that has been used once reads back addr == 0 until the driver
     * re-arms it.  With an 8-BD ring, THE NINTH FRAME OF A BURST WRAPS ONTO A
     * DESCRIPTOR THE GUEST HAS NOT RE-ARMED, reads addr = 0, and we DMA the frame
     * STRAIGHT INTO GUEST ADDRESS ZERO -- while still stamping the descriptor READY,
     * so the driver copies a stale buffer and believes it.
     *
     * And the burst is delivered by our OWN fix for the RX stall: the
     * qemu_flush_queued_packets() below releases everything the socket queued while
     * can_receive() was false, in one go, into a ring that had no way to say no.
     * THE REPAIR AND THE TRIGGER WERE THE SAME COMMIT.
     *
     * Measured on the fleet's staggered 3-node L2 lab (holobench, 2026-07-13), where
     * a node joins a segment that is ALREADY CARRYING TRAFFIC:
     *
     *     node booting into an EMPTY segment   ->  0 frames to address 0
     *     node joining second                  ->  8
     *     node joining LAST, 20s of live wire  -> 88     ... and it still printed PASS,
     *                                                        reporting "peers" whose
     *                                                        source MAC was ITS OWN.
     *
     * A synchronous lab holds that quantity at zero, so it is not merely blind to
     * this -- it is blind BY CONSTRUCTION.
     *
     * Real silicon DROPS a frame it has no descriptor for (and bumps a discard
     * counter).  It does not write into a descriptor the driver still owns.  So do
     * we.  Dropping is also the honest choice against the alternative of applying
     * backpressure here: a wire has no backpressure, and a model that is more
     * forgiving than the silicon ships the bug downstream.
     */
    cir = netc_reg(s, R_RBCIR) & 0xFFFF;
    if (cir >= rlen || ((pir + 1) % rlen) == cir) {
        s->rx_ring_full_drops++;
        /*
         * ONGOING, NOT FIRST-ONLY.  This used to log only on drop #1 -- so a startup
         * transient consumed the one message, and a LATER burst (e.g. a peer rejoining a
         * loaded segment while the ring is chronically full) dropped frames SILENTLY.
         *
         * holobench, 2026-07-15, on rt1180's 47s peer re-acquire: "does the node log RX
         * ring full during the gap?"  With first-only logging the honest answer was "it
         * couldn't tell you" -- the instrument was blind exactly when the finding needed
         * it.  ⭐ A DROP COUNTER THAT ANNOUNCES ONCE CANNOT TESTIFY ABOUT A BURST.  So log
         * the first, then periodically with the RUNNING TOTAL, so a sustained ring-full
         * during a rejoin shows up as rising counts on the (-d guest_errors) log the lab
         * can capture.  (mcxn947qemu is adding the on-the-wire version on their 1-deep
         * ring; this is the same discipline on our side: let the subject testify.)
         */
        if (s->rx_ring_full_drops == 1 || (s->rx_ring_full_drops % 64u) == 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "imxrt1180-netc: RX ring full (pir=%u cir=%u len=%u) -- frame "
                          "dropped (total %u). The guest is not consuming descriptors as "
                          "fast as the wire delivers them.\n",
                          pir, cir, rlen, (unsigned)s->rx_ring_full_drops);
        }
        return;
    }

    bd = base + (hwaddr)pir * 16;
    dma_memory_read(s->dma_as, bd, sbd, 8, MEMTXATTRS_UNSPECIFIED);
    buf = ldq_le_p(sbd);                         /* standard.addr (SW-posted) */
    dma_memory_write(s->dma_as, buf, frame, len, MEMTXATTRS_UNSPECIFIED);

    memset(wb, 0, sizeof(wb));
    stw_le_p(wb + 8, len);                       /* writeback.bufLen */
    wb[14] = 0;                                  /* error */
    wb[15] = RXBD_WB_READY | RXBD_WB_FINAL;      /* isReady | isFinal */
    dma_memory_write(s->dma_as, bd, wb, 16, MEMTXATTRS_UNSPECIFIED);

    netc_backing_write(s, R_RBPIR, (pir + 1) % rlen, 4);
    netc_emit_msix(s, netc_reg(s, R_SIMSIRRVR0));
}

/* TX producer written: walk the new TX BDs, form each frame, write the BD back
 * done, loop it into the RX ring, then emit the TX MSI-X. */
/*
 * Switch logical port numbers used for source-MAC learning.  On the SW0 switch
 * the CPU reaches the fabric through the ENETC management pseudo-port (port 4);
 * the external wire is a physical port (port 0).  An untagged frame uses the
 * default filtering-ID 0 (a real switch derives the FID from the ingress port's
 * default VID via the VLAN filter table; untagged with no VLAN config => 0).
 */
#define NETC_SW_PORT_CPU    4
#define NETC_SW_PORT_WIRE   0
#define NETC_SW_DEFAULT_FID 0
#define NETC_SW_NUM_PORTS   5
#define NETC_SW_ALL_PORTS   0x1Fu
/* Switch port 0 (the wire) MAC "Transmit Frame Counter" (aFramesTransmittedOK):
 * ETH_LINK @ 0x60A05000, PM0_TFRMN @ +0x220 (64-bit). */
#define R_SW_P0_TFRMN       0xA05220
static void netc_switch_learn(IMXRT1180NETCState *s, const uint8_t *src_mac,
                              uint16_t fid, unsigned port);
static uint32_t netc_switch_egress(IMXRT1180NETCState *s, const uint8_t *dest_mac,
                                   uint16_t fid, unsigned ingress_port);

static void netc_do_tx(IMXRT1180NETCState *s)
{
    uint64_t base = (uint64_t)netc_reg(s, R_TBBAR0) | ((uint64_t)netc_reg(s, R_TBBAR1) << 32);
    uint32_t tlen = netc_reg(s, R_TBLENR) & BDR_LEN_MASK;   /* #BDs */
    uint32_t cir = netc_reg(s, R_TBCIR) & 0xFFFF;
    uint32_t pir = netc_reg(s, R_TBPIR) & 0xFFFF;

    if (tlen == 0) {
        return;
    }
    while (cir != pir) {
        hwaddr bd = base + (hwaddr)cir * 16;
        uint8_t txbd[16], frame[NETC_FRAME_MAX];
        uint64_t addr;
        uint32_t flen, wb;

        dma_memory_read(s->dma_as, bd, txbd, 16, MEMTXATTRS_UNSPECIFIED);
        addr = ldq_le_p(txbd);
        flen = lduw_le_p(txbd + 10);             /* standard.frameLen */
        if (flen == 0 || flen > NETC_FRAME_MAX) {
            flen = lduw_le_p(txbd + 8);          /* fall back to bufLen */
        }
        if (flen > NETC_FRAME_MAX) {
            flen = NETC_FRAME_MAX;
        }
        dma_memory_read(s->dma_as, addr, frame, flen, MEMTXATTRS_UNSPECIFIED);

        wb = cpu_to_le32(TXBD_WB_WRITTEN);       /* written=1, status=success */
        dma_memory_write(s->dma_as, bd + 8, &wb, 4, MEMTXATTRS_UNSPECIFIED);

        /* Switch ingress on the CPU/management port: learn the source MAC. */
        if (flen >= 14) {
            netc_switch_learn(s, frame + 6, NETC_SW_DEFAULT_FID, NETC_SW_PORT_CPU);
        }

        if (s->phy_regs[0] & PHY_BMCR_LOOPBACK) {
            /* PHY local loopback: the frame U-turns back into our own RX ring
             * (this is what the SDK netc_txrx_transfer example relies on).  This is
             * a MAC self-test, below the switch -- it bypasses the forwarding path. */
            netc_deliver_rx(s, frame, flen);
        } else if (flen >= 14) {
            /*
             * Switch egress: the CPU-injected frame ingresses on the management
             * port; the switch forwards it out the ports its destination resolves
             * to.  We have one physical port (the wire), so a frame egresses the
             * wire iff the destination floods (unknown unicast / broadcast) or its
             * FDB entry names the wire port.  A destination that maps only to the
             * CPU's own port is dropped by split-horizon -- NOT put on the wire.
             * An unprogrammed FDB floods, so plain endpoint TX still reaches the
             * wire exactly as before.
             */
            uint32_t egress = netc_switch_egress(s, frame, NETC_SW_DEFAULT_FID,
                                                 NETC_SW_PORT_CPU);
            if (egress & (1u << NETC_SW_PORT_WIRE)) {
                qemu_send_packet(qemu_get_queue(s->nic), frame, flen);
                /* count it out the wire port's MAC (PM0_TFRMN, aFramesTransmittedOK) */
                netc_backing_write(s, R_SW_P0_TFRMN,
                                   netc_backing_read(s, R_SW_P0_TFRMN, 8) + 1, 8);
            }
            /* else: forwarded elsewhere / dropped by the switch -- not to the wire */
        }
        cir = (cir + 1) % tlen;
    }
    netc_backing_write(s, R_TBCIR, cir, 4);
    netc_emit_msix(s, netc_reg(s, R_SIMSITRVR0));
}

/* ---- Ethernet backend (netdev) ------------------------------------------ */
static bool netc_can_receive(NetClientState *nc)
{
    IMXRT1180NETCState *s = qemu_get_nic_opaque(nc);
    /*
     * READY MEANS "THE GUEST HAS ENABLED THE RING", NOT "THE GUEST HAS SIZED IT".
     *
     * This used to gate on RBLENR != 0, and that is TOO EARLY BY SEVERAL STEPS.
     * The driver's ring bring-up is, in order (fsl_netc_hw_si.c:63-75, then
     * fsl_netc_endpoint.c:163, then fsl_netc_hw_si.h:119):
     *
     *     RBBAR0/1 = ring base
     *     RBPIR = 0 ; RBCIR = 0
     *     RBLENR = len            <-- we used to declare ourselves READY here
     *     RBMR   = mode bits
     *     ... the EP layer now posts standard.addr into EVERY descriptor ...
     *     RBMR |= EN              <-- the ring is ACTUALLY armed only here
     *
     * Gating on RBLENR meant we began delivering into a ring whose descriptors had
     * NO BUFFER ADDRESS YET -- addr reads back 0 -- so we DMA'd frames straight into
     * GUEST ADDRESS ZERO and stamped the descriptors READY over the driver's own
     * half-finished setup. Measured on the fleet's staggered 3-node lab: a node
     * joining a segment already carrying traffic took 88 such frames and still
     * printed PASS, reporting "peers" whose source MAC was its own.
     *
     * EN is the guest's own statement that the buffers are posted. Believe that, and
     * nothing else.
     */
    return (netc_reg(s, R_RBMR) & RBMR_EN) != 0;
}

static ssize_t netc_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    IMXRT1180NETCState *s = qemu_get_nic_opaque(nc);
    uint32_t len = size > NETC_FRAME_MAX ? NETC_FRAME_MAX : (uint32_t)size;

    /* Switch ingress on the physical (wire) port: learn the source MAC. */
    if (len >= 14) {
        netc_switch_learn(s, buf + 6, NETC_SW_DEFAULT_FID, NETC_SW_PORT_WIRE);
    }

    /* Inbound frame from the wire -> into the RX ring (+ RX MSI-X). */
    netc_deliver_rx(s, buf, len);
    return size;
}

static NetClientInfo netc_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = netc_can_receive,
    .receive = netc_receive,
};

/* ============ NETC switch: NTMP command-BD ring + FDB table ================
 *
 * The switch driver (fsl_netc_switch + fsl_netc_hw) programs the L2 forwarding
 * database and VLAN filter over a "command BD ring" using the NETC Table
 * Management Protocol (NTMP).  Per operation it writes a 32-byte command BD into
 * the ring, then rings the doorbell CBDRPIR = (index+1)%len and SPINS on
 *   while (producerIndex != CBDRCIR)
 * -- there is no busy bit; completion IS hardware advancing CBDRCIR to the
 * producer index (NETC_CmdBDSendCommand, fsl_netc_hw.c).  We process the BD
 * synchronously on the CBDRPIR write and set CBDRCIR = CBDRPIR, releasing the
 * spin.  A command that we do not model must fault through the BD's own error
 * field (resp.error), never by leaving CBDRCIR behind -- that would HANG the
 * driver instead of telling it.
 *
 * Register offsets/masks: PERI_NETC_SW.h (SW0 @ 0x60A00000).  BD + table byte
 * offsets: fsl_netc.h, compiler-verified.
 */
#define SW0_OFF            0xA00000               /* SW0_BASE - NETC base */
#define SW_CBDR_BASE       (SW0_OFF + 0x800)
#define SW_CBDR_STEP       0x30
#define SW_NUM_CBDR        2
#define SWCBDR_MR          0x00                   /* mode; EN = bit 31 */
#define SWCBDR_BAR0        0x10                   /* ring base low (128B-aligned) */
#define SWCBDR_BAR1        0x14                   /* ring base high */
#define SWCBDR_PIR         0x18                   /* producer index (doorbell) */
#define SWCBDR_CIR         0x1C                   /* consumer index (completion) */
#define SWCBDR_LENR        0x20                   /* ring length (#BDs, mult of 8) */
#define SWCBDR_MR_EN       0x80000000u
#define SWCBDR_LEN_MASK    0x7F8u
#define SWCBDR_IDX_MASK    0x3FFu
#define SWCBDR_BAR0_MASK   0xFFFFFF80u

#define NTMP_BD_SIZE       32
/* command BD dword@12: cmd[3:0], accessType[13:12], tableId[23:16]; resp view of
 * the same dword: numMatched[15:0], error[27:16], resReady[31]. */
#define NTMP_TB_FDB        15
#define NTMP_TB_VF         18                      /* VLAN filter table */
#define NTMP_CMD_DELETE    0x1
#define NTMP_CMD_UPDATE    0x2
#define NTMP_CMD_QUERY     0x4
#define NTMP_CMD_ADD       0x8
#define NTMP_ACC_ENTRYID   0                      /* kNETC_EntryIDMatch */
/* NTMP error status (netc_cmd_error_t) -- documented codes, not invented. */
#define NTMP_ERR_NONE      0x00
#define NTMP_ERR_SIZE      0x02                   /* kNETC_SizeError: table full */
#define NTMP_ERR_INV_TABLE 0x80                   /* kNETC_InvTableID */

static IMXRT1180NETCFdbEntry *netc_fdb_find_key(IMXRT1180NETCState *s,
                                                const uint8_t *mac, uint16_t fid)
{
    for (int i = 0; i < IMXRT1180_NETC_FDB_SIZE; i++) {
        IMXRT1180NETCFdbEntry *e = &s->fdb[i];
        if (e->valid && e->fid == fid && memcmp(e->mac, mac, 6) == 0) {
            return e;
        }
    }
    return NULL;
}

static IMXRT1180NETCFdbEntry *netc_fdb_find_id(IMXRT1180NETCState *s, uint32_t id)
{
    for (int i = 0; i < IMXRT1180_NETC_FDB_SIZE; i++) {
        if (s->fdb[i].valid && s->fdb[i].entry_id == id) {
            return &s->fdb[i];
        }
    }
    return NULL;
}

/*
 * Source-MAC learning.  A frame ingressing on `port` teaches the switch that its
 * source MAC is reachable via that port: create or refresh a DYNAMIC FDB entry.
 * This is how a real switch populates its forwarding database from live traffic
 * (the NTMP-programmed entries are the STATIC ones).  Rules:
 *   - a multicast/broadcast source address is never a real station -> ignore it;
 *   - never disturb a STATIC entry (operator config wins over learning);
 *   - a known dynamic MAC seen on a new port has moved -> update its port;
 *   - if the table is full, stop learning silently (as the hardware does -- it is
 *     not a command error, just a full CAM).
 */
static void netc_switch_learn(IMXRT1180NETCState *s, const uint8_t *src_mac,
                              uint16_t fid, unsigned port)
{
    IMXRT1180NETCFdbEntry *e;

    if (src_mac[0] & 0x01) {
        return;                            /* group address: not a source */
    }
    e = netc_fdb_find_key(s, src_mac, fid);
    if (e) {
        if (e->dynamic) {
            e->port_bitmap = 1u << port;   /* station moved to a new port */
        }
        return;                            /* static entry: leave it alone */
    }
    for (int i = 0; i < IMXRT1180_NETC_FDB_SIZE; i++) {
        if (!s->fdb[i].valid) {
            e = &s->fdb[i];
            memcpy(e->mac, src_mac, 6);
            e->fid = fid;
            e->port_bitmap = 1u << port;
            e->dynamic = true;
            e->cfge_flags = (1u << 11);    /* cfge.dynamic bit set */
            e->et_eid = 0;
            e->valid = true;
            e->entry_id = s->fdb_next_id++;
            return;
        }
    }
}

/*
 * Switch forwarding decision: the egress port bitmap for a frame with destination
 * `dest_mac` and filtering-ID `fid` ingressing on `ingress_port`.
 *   - a group destination (broadcast/multicast) floods to all ports;
 *   - a unicast destination with an FDB entry egresses that entry's port bitmap;
 *   - an unknown unicast floods (so an unprogrammed switch behaves as a hub, which
 *     is what a plain endpoint relies on);
 *   - the result is intersected with the VLAN's port membership if this fid has a
 *     VLAN filter entry, and never includes the ingress port (split-horizon).
 */
static uint32_t netc_switch_egress(IMXRT1180NETCState *s, const uint8_t *dest_mac,
                                   uint16_t fid, unsigned ingress_port)
{
    uint32_t bitmap;

    if (dest_mac[0] & 0x01) {
        bitmap = NETC_SW_ALL_PORTS;              /* group address -> flood */
    } else {
        IMXRT1180NETCFdbEntry *e = netc_fdb_find_key(s, dest_mac, fid);
        bitmap = e ? e->port_bitmap : NETC_SW_ALL_PORTS;   /* unknown unicast -> flood */
    }
    for (int i = 0; i < IMXRT1180_NETC_VF_SIZE; i++) {
        if (s->vlan[i].valid && (s->vlan[i].cfge[1] & 0xFFF) == fid) {
            bitmap &= s->vlan[i].cfge[0] & 0xFFFFFF;   /* restrict to VLAN members */
            break;
        }
    }
    return bitmap & ~(1u << ingress_port);       /* split-horizon */
}

/*
 * Execute one FDB (tableId 15) command.  Buffer layout at req_addr
 * (compiler-verified offsets from netc_tb_fdb_{req,rsp}_data_t):
 *   request  (48B): commonHeader@0; union@4 { entryID@4 | keye@4:
 *                   mac[6]@4, fid dword@12 [11:0] }; cfge@36:
 *                   portBitmap dword@36 [23:0], flags dword@40, etEID@44.
 *   response (36B): status@0, entryID@4, keye@8 (mac@8, fid dword@16),
 *                   cfge@20 (portBitmap@20, flags@24, etEID@28), acte@32.
 * cmd is a bitfield (Add 0x8, Update 0x2, Query 0x4, Delete 0x1) so combined
 * ops (AddAndQuery 0xC, QueryAndDelete 0x5) fall out of testing each bit.
 * Returns the NTMP error status and sets *num_matched.
 */
static uint32_t netc_fdb_op(IMXRT1180NETCState *s, unsigned cmd, unsigned acc,
                            uint64_t req_addr, uint16_t *num_matched)
{
    uint8_t rbuf[48];
    IMXRT1180NETCFdbEntry *e = NULL;
    uint32_t err = NTMP_ERR_NONE;

    *num_matched = 0;
    dma_memory_read(s->dma_as, req_addr, rbuf, sizeof(rbuf), MEMTXATTRS_UNSPECIFIED);

    if (cmd & NTMP_CMD_ADD) {
        uint16_t fid = ldl_le_p(rbuf + 12) & 0xFFF;
        e = netc_fdb_find_key(s, rbuf + 4, fid);
        if (!e) {
            for (int i = 0; i < IMXRT1180_NETC_FDB_SIZE && !e; i++) {
                if (!s->fdb[i].valid) {
                    e = &s->fdb[i];
                }
            }
            if (!e) {
                return NTMP_ERR_SIZE;            /* table full -- honest fault */
            }
            memcpy(e->mac, rbuf + 4, 6);
            e->fid = fid;
            e->valid = true;
            e->entry_id = s->fdb_next_id++;
        }
        e->port_bitmap = ldl_le_p(rbuf + 36) & 0xFFFFFF;
        e->cfge_flags  = ldl_le_p(rbuf + 40);
        e->et_eid      = ldl_le_p(rbuf + 44);
        e->dynamic     = (e->cfge_flags >> 11) & 1;
        *num_matched = 1;
    } else if (cmd & NTMP_CMD_UPDATE) {
        e = (acc == NTMP_ACC_ENTRYID)
              ? netc_fdb_find_id(s, ldl_le_p(rbuf + 4))
              : netc_fdb_find_key(s, rbuf + 4, ldl_le_p(rbuf + 12) & 0xFFF);
        if (e) {
            e->port_bitmap = ldl_le_p(rbuf + 36) & 0xFFFFFF;
            e->cfge_flags  = ldl_le_p(rbuf + 40);
            e->et_eid      = ldl_le_p(rbuf + 44);
            e->dynamic     = (e->cfge_flags >> 11) & 1;
            *num_matched = 1;
        }
    }

    if (cmd & NTMP_CMD_QUERY) {
        if (!e) {
            e = (acc == NTMP_ACC_ENTRYID)
                  ? netc_fdb_find_id(s, ldl_le_p(rbuf + 4))
                  : netc_fdb_find_key(s, rbuf + 4, ldl_le_p(rbuf + 12) & 0xFFF);
        }
        if (e) {
            uint8_t resp[36];
            memset(resp, 0, sizeof(resp));
            stl_le_p(resp + 4, e->entry_id);      /* rsp.entryID */
            memcpy(resp + 8, e->mac, 6);          /* rsp.keye.macAddr */
            stl_le_p(resp + 16, e->fid & 0xFFF);  /* rsp.keye.fid */
            stl_le_p(resp + 20, e->port_bitmap);  /* rsp.cfge.portBitmap */
            stl_le_p(resp + 24, e->cfge_flags);   /* rsp.cfge flags */
            stl_le_p(resp + 28, e->et_eid);       /* rsp.cfge.etEID */
            dma_memory_write(s->dma_as, req_addr, resp, sizeof(resp),
                             MEMTXATTRS_UNSPECIFIED);
            *num_matched = 1;
        }
    }

    if (cmd & NTMP_CMD_DELETE) {
        IMXRT1180NETCFdbEntry *d = (acc == NTMP_ACC_ENTRYID)
              ? netc_fdb_find_id(s, ldl_le_p(rbuf + 4))
              : netc_fdb_find_key(s, rbuf + 4, ldl_le_p(rbuf + 12) & 0xFFF);
        if (d) {
            d->valid = false;
            *num_matched = 1;
        }
    }

    return err;
}

static IMXRT1180NETCVlanEntry *netc_vlan_find_vid(IMXRT1180NETCState *s, uint16_t vid)
{
    for (int i = 0; i < IMXRT1180_NETC_VF_SIZE; i++) {
        if (s->vlan[i].valid && s->vlan[i].vid == vid) {
            return &s->vlan[i];
        }
    }
    return NULL;
}

static IMXRT1180NETCVlanEntry *netc_vlan_find_id(IMXRT1180NETCState *s, uint32_t id)
{
    for (int i = 0; i < IMXRT1180_NETC_VF_SIZE; i++) {
        if (s->vlan[i].valid && s->vlan[i].entry_id == id) {
            return &s->vlan[i];
        }
    }
    return NULL;
}

/*
 * Execute one VLAN-filter (tableId 18) command.  Buffer layout at req_addr
 * (compiler-verified from netc_tb_vf_{req,rsp}_data_t):
 *   request  (24B): commonHeader@0; union@4 { entryID@4 | keye@4: vid dword@4
 *                   [11:0] }; cfge@8 (16 raw bytes: portMembership@8 [23:0],
 *                   fid dword@12 [11:0], etaBitmap@16, baseETEID@20).
 *   response (28B): status@0, entryID@4, keye@8 (vid dword@8), cfge@12 (16 bytes).
 * The VLAN filter maps a VID to a filtering-ID (FID) and a VLAN port membership;
 * the FDB lookup's fid comes from here.  Same NTMP mechanism as the FDB.
 */
static uint32_t netc_vf_op(IMXRT1180NETCState *s, unsigned cmd, unsigned acc,
                           uint64_t req_addr, uint16_t *num_matched)
{
    uint8_t rbuf[24];
    IMXRT1180NETCVlanEntry *e = NULL;

    *num_matched = 0;
    dma_memory_read(s->dma_as, req_addr, rbuf, sizeof(rbuf), MEMTXATTRS_UNSPECIFIED);

    if (cmd & NTMP_CMD_ADD) {
        uint16_t vid = ldl_le_p(rbuf + 4) & 0xFFF;
        e = netc_vlan_find_vid(s, vid);
        if (!e) {
            for (int i = 0; i < IMXRT1180_NETC_VF_SIZE && !e; i++) {
                if (!s->vlan[i].valid) {
                    e = &s->vlan[i];
                }
            }
            if (!e) {
                return NTMP_ERR_SIZE;
            }
            e->vid = vid;
            e->valid = true;
            e->entry_id = s->vlan_next_id++;
        }
        for (int i = 0; i < 4; i++) {
            e->cfge[i] = ldl_le_p(rbuf + 8 + i * 4);
        }
        *num_matched = 1;
    } else if (cmd & NTMP_CMD_UPDATE) {
        e = (acc == NTMP_ACC_ENTRYID)
              ? netc_vlan_find_id(s, ldl_le_p(rbuf + 4))
              : netc_vlan_find_vid(s, ldl_le_p(rbuf + 4) & 0xFFF);
        if (e) {
            for (int i = 0; i < 4; i++) {
                e->cfge[i] = ldl_le_p(rbuf + 8 + i * 4);
            }
            *num_matched = 1;
        }
    }

    if (cmd & NTMP_CMD_QUERY) {
        if (!e) {
            e = (acc == NTMP_ACC_ENTRYID)
                  ? netc_vlan_find_id(s, ldl_le_p(rbuf + 4))
                  : netc_vlan_find_vid(s, ldl_le_p(rbuf + 4) & 0xFFF);
        }
        if (e) {
            uint8_t resp[28];
            memset(resp, 0, sizeof(resp));
            stl_le_p(resp + 4, e->entry_id);      /* rsp.entryID */
            stl_le_p(resp + 8, e->vid & 0xFFF);   /* rsp.keye.vid */
            for (int i = 0; i < 4; i++) {
                stl_le_p(resp + 12 + i * 4, e->cfge[i]);  /* rsp.cfge (16 bytes) */
            }
            dma_memory_write(s->dma_as, req_addr, resp, sizeof(resp),
                             MEMTXATTRS_UNSPECIFIED);
            *num_matched = 1;
        }
    }

    if (cmd & NTMP_CMD_DELETE) {
        IMXRT1180NETCVlanEntry *d = (acc == NTMP_ACC_ENTRYID)
              ? netc_vlan_find_id(s, ldl_le_p(rbuf + 4))
              : netc_vlan_find_vid(s, ldl_le_p(rbuf + 4) & 0xFFF);
        if (d) {
            d->valid = false;
            *num_matched = 1;
        }
    }

    return NTMP_ERR_NONE;
}

/* Process one 32-byte NTMP command BD at bd_addr: dispatch on tableId/cmd, then
 * write the response (error/numMatched/resReady) back into the BD's dword@12. */
static void netc_process_cmd_bd(IMXRT1180NETCState *s, uint64_t bd_addr)
{
    uint8_t bd[NTMP_BD_SIZE];
    uint64_t req_addr;
    uint32_t dw3, err;
    unsigned cmd, acc, table_id;
    uint16_t num_matched = 0;

    dma_memory_read(s->dma_as, bd_addr, bd, sizeof(bd), MEMTXATTRS_UNSPECIFIED);
    req_addr = ldq_le_p(bd);                      /* req.addr (data buffer) */
    dw3      = ldl_le_p(bd + 12);
    cmd      = dw3 & 0xF;
    acc      = (dw3 >> 12) & 0x3;
    table_id = (dw3 >> 16) & 0xFF;

    switch (table_id) {
    case NTMP_TB_FDB:
        err = netc_fdb_op(s, cmd, acc, req_addr, &num_matched);
        break;
    case NTMP_TB_VF:
        err = netc_vf_op(s, cmd, acc, req_addr, &num_matched);
        break;
    default:
        /* We do not model this table yet.  TELL THE DRIVER via the BD's own error
         * field (a documented, non-gating channel) instead of faking success -- a
         * silent ack over an unprogrammed table is a lie the driver cannot see. */
        qemu_log_mask(LOG_UNIMP, "imxrt1180-netc: NTMP command for unmodelled "
                      "table %u (cmd 0x%x) -- returning kNETC_InvTableID (flagged)\n",
                      table_id, cmd);
        err = NTMP_ERR_INV_TABLE;
        break;
    }

    /* Response into dword@12: numMatched[15:0], error[27:16], resReady[31]. */
    stl_le_p(bd + 12, (num_matched & 0xFFFF) | ((err & 0xFFF) << 16) | (1u << 31));
    dma_memory_write(s->dma_as, bd_addr + 12, bd + 12, 4, MEMTXATTRS_UNSPECIFIED);
}

/* CBDRPIR (doorbell) written for a ring: process every BD between the consumer
 * index and the new producer index, then advance CBDRCIR to release the driver's
 * spin-wait (which polls CBDRCIR == producerIndex). */
static void netc_cbdr_doorbell(IMXRT1180NETCState *s, unsigned ring)
{
    hwaddr rbase = SW_CBDR_BASE + (hwaddr)ring * SW_CBDR_STEP;
    uint64_t ring_base;
    uint32_t len, pir, cir, guard = 0;

    if (!(netc_reg(s, rbase + SWCBDR_MR) & SWCBDR_MR_EN)) {
        return;                                   /* ring disabled */
    }
    len = netc_reg(s, rbase + SWCBDR_LENR) & SWCBDR_LEN_MASK;
    if (len == 0) {
        return;
    }
    ring_base = (uint64_t)(netc_reg(s, rbase + SWCBDR_BAR0) & SWCBDR_BAR0_MASK) |
                ((uint64_t)netc_reg(s, rbase + SWCBDR_BAR1) << 32);
    pir = netc_reg(s, rbase + SWCBDR_PIR) & SWCBDR_IDX_MASK;
    cir = netc_reg(s, rbase + SWCBDR_CIR) & SWCBDR_IDX_MASK;

    while (cir != pir && guard++ <= len) {
        netc_process_cmd_bd(s, ring_base + (uint64_t)cir * NTMP_BD_SIZE);
        cir = (cir + 1) % len;
    }
    netc_backing_write(s, rbase + SWCBDR_CIR, pir, 4);   /* completion */
}

/* Is off a CBDRPIR doorbell register for some switch command BD ring? */
static bool netc_is_cbdr_pir(hwaddr off, unsigned *ring)
{
    for (unsigned r = 0; r < SW_NUM_CBDR; r++) {
        if (off == SW_CBDR_BASE + (hwaddr)r * SW_CBDR_STEP + SWCBDR_PIR) {
            *ring = r;
            return true;
        }
    }
    return false;
}

/* ============ PTP 1588 timer (TMR0 @ 0x60B80000) ==========================
 *
 * The IEEE-1588 timer is a digital DDS: each reference-clock tick the addend is
 * accumulated, and the nanosecond counter advances by addend/2^32 ns; the driver
 * tunes frequency by scaling the addend (NETC_TimerAdjustFreq: addend =
 * 2^32*(1e9+ppb)/timerFreq).  At ppb=0 the counter therefore advances exactly one
 * nanosecond per real nanosecond -- timerFreq cancels.  We model that against the
 * QEMU virtual clock: the count = base + elapsed_virtual_ns * (addend / nominal),
 * where the FIRST addend written is taken as the rate-1 nominal, so subsequent
 * ppb adjustments scale the rate faithfully without needing the (runtime-derived)
 * reference frequency.  Software reads the 64-bit time through TMR_CUR_TIME_H/L
 * with an H-L-H coherency loop; reading _L latches _H. */
#define TMR0_OFF        0xB80000
#define R_TMR_DEFCNT_L  (TMR0_OFF + 0x30)
#define R_TMR_DEFCNT_H  (TMR0_OFF + 0x34)
#define R_TMR_CTRL      (TMR0_OFF + 0x80)
#define R_TMR_CNT_L     (TMR0_OFF + 0x98)
#define R_TMR_CNT_H     (TMR0_OFF + 0x9C)
#define R_TMR_ADD       (TMR0_OFF + 0xA0)
#define R_TMROFF_L      (TMR0_OFF + 0xB0)
#define R_TMROFF_H      (TMR0_OFF + 0xB4)
#define R_TMR_CUR_L     (TMR0_OFF + 0xF0)
#define R_TMR_CUR_H     (TMR0_OFF + 0xF4)
#define TMR_CTRL_TE               0x4u
#define TMR_CTRL_TCLK_PERIOD_MASK 0x3FF0000u

/* Full 64-bit addend = TCLK_PERIOD (TMR_CTRL[25:16]) << 32 | TMR_ADD. */
static uint64_t netc_ptp_addend(IMXRT1180NETCState *s)
{
    uint32_t tclk = (netc_reg(s, R_TMR_CTRL) & TMR_CTRL_TCLK_PERIOD_MASK) >> 16;
    return ((uint64_t)tclk << 32) | netc_reg(s, R_TMR_ADD);
}

/* The raw nanosecond counter (before the software offset). */
static uint64_t netc_ptp_raw(IMXRT1180NETCState *s)
{
    int64_t now, dt;
    uint64_t fa, nom;

    if (!s->ptp_enabled) {
        return s->ptp_cnt_base;                  /* frozen while disabled */
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    dt = now - s->ptp_t_base;
    if (dt < 0) {
        dt = 0;
    }
    fa = netc_ptp_addend(s);
    nom = s->ptp_nominal_addend;
    if (nom == 0 || fa == nom) {
        return s->ptp_cnt_base + (uint64_t)dt;   /* rate 1.0 (exact) */
    }
    return s->ptp_cnt_base + (uint64_t)((double)dt * (double)fa / (double)nom);
}

/* Current time = raw counter + the signed software offset (TMROFF). */
static uint64_t netc_ptp_cur_time(IMXRT1180NETCState *s)
{
    int64_t off = (int64_t)(((uint64_t)netc_reg(s, R_TMROFF_H) << 32) |
                            netc_reg(s, R_TMROFF_L));
    return netc_ptp_raw(s) + (uint64_t)off;
}

/* Freeze the running count into the base and reset the clock origin, so a
 * config change (enable/disable, addend, counter set) takes effect from now. */
static void netc_ptp_relatch(IMXRT1180NETCState *s)
{
    s->ptp_cnt_base = netc_ptp_raw(s);
    s->ptp_t_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* Handle a write to a TMR0 register; returns true if it was one. */
static bool netc_ptp_write(IMXRT1180NETCState *s, hwaddr off, uint64_t val,
                           unsigned size)
{
    switch (off) {
    case R_TMR_CTRL:
        netc_ptp_relatch(s);                     /* freeze with the OLD config */
        netc_backing_write(s, off, val, size);
        s->ptp_enabled = (netc_reg(s, R_TMR_CTRL) & TMR_CTRL_TE) != 0;
        return true;
    case R_TMR_ADD:
        netc_ptp_relatch(s);
        netc_backing_write(s, off, val, size);
        if (s->ptp_nominal_addend == 0) {
            s->ptp_nominal_addend = netc_ptp_addend(s);  /* first addend = rate 1 */
        }
        return true;
    case R_TMR_CNT_L:
    case R_TMR_CNT_H:
        netc_backing_write(s, off, val, size);   /* software sets the counter */
        s->ptp_cnt_base = ((uint64_t)netc_reg(s, R_TMR_CNT_H) << 32) |
                          netc_reg(s, R_TMR_CNT_L);
        s->ptp_t_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        return true;
    case R_TMROFF_L:
    case R_TMROFF_H:
        netc_backing_write(s, off, val, size);   /* offset applied at read */
        return true;
    default:
        return false;
    }
}

/* Handle a read from a TMR0 register; sets *out and returns true if it was one. */
static bool netc_ptp_read(IMXRT1180NETCState *s, hwaddr off, uint64_t *out)
{
    switch (off) {
    case R_TMR_CUR_L: {
        uint64_t t = netc_ptp_cur_time(s);
        s->ptp_cur_hi_latch = (uint32_t)(t >> 32);   /* reading _L latches _H */
        *out = (uint32_t)t;
        return true;
    }
    case R_TMR_CUR_H:
        *out = s->ptp_cur_hi_latch;
        return true;
    case R_TMR_CNT_L:
        *out = (uint32_t)netc_ptp_raw(s);
        return true;
    case R_TMR_CNT_H:
        *out = (uint32_t)(netc_ptp_raw(s) >> 32);
        return true;
    case R_TMR_DEFCNT_L:
        *out = (uint32_t)s->ptp_cnt_base;            /* frozen "default" count */
        return true;
    case R_TMR_DEFCNT_H:
        *out = (uint32_t)(s->ptp_cnt_base >> 32);
        return true;
    default:
        return false;
    }
}

static uint64_t netc_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180NETCState *s = opaque;
    uint64_t ptp;

    if (netc_is_pci_flr(off)) {
        /* INIT_FLR always reads clear: the FLR-complete poll exits at once. */
        return netc_backing_read(s, off, size) & ~(uint64_t)PCI_INIT_FLR;
    }
    if (netc_ptp_read(s, off, &ptp)) {
        return ptp;                                     /* PTP 1588 timer register */
    }

    switch (off) {
    case R_NETCSR:   return 0;                          /* STATE/ERROR clear */
    case R_NETCRR:   return netc_backing_read(s, off, size) & ~(uint64_t)NETCRR_SR;
    case R_ECAPR0:   return 0;
    case R_ECAPR1:   return ECAPR1_VAL;
    case R_ECAPR2:   return ECAPR2_VAL;
    case R_SIPCAPR1: return SIPCAPR1_VAL;
    case R_EMDIO_CFG: return netc_backing_read(s, off, size) & ~(uint64_t)EMDIO_CFG_BSY;
    case R_EMDIO_DATA: return netc_phy_read(s, s->mdio_reg);
    case R_PM0_CMD_CFG:
    case R_PM1_CMD_CFG:                                  /* MAC SWR self-clears */
        return netc_backing_read(s, off, size) & ~(uint64_t)PM_CMD_CFG_SWR;
    default:
        return netc_backing_read(s, off, size);
    }
}

static void netc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IMXRT1180NETCState *s = opaque;
    unsigned ring;

    /*
     * NTMP command-BD doorbell: the switch driver writes CBDRPIR to launch a
     * table (FDB/VLAN) operation, then spins on CBDRCIR.  Store the index, run
     * the command(s), and advance CBDRCIR to release the spin.
     */
    if (netc_is_cbdr_pir(off, &ring)) {
        netc_backing_write(s, off, val, size);
        netc_cbdr_doorbell(s, ring);
        return;
    }

    if (netc_ptp_write(s, off, val, size)) {
        return;                                     /* PTP 1588 timer register */
    }

    /*
     * RX-QUEUE RESTART.
     *
     * netc_can_receive() reports "not ready" until the guest has configured the RX
     * ring (RBLENR != 0). When a NetClientInfo.can_receive returns false, QEMU
     * STALLS that peer's queue and does NOT retry on its own -- the device must
     * call qemu_flush_queued_packets() once it can accept again. We never did.
     *
     * So a frame arriving in the window between the NIC coming up and the guest
     * programming its RX ring stalled the queue PERMANENTLY: that instance never
     * received another frame, for the life of the run.
     *
     * Invisible in every 2-node test we had, because both ends boot together and
     * neither transmits before the other is listening. It took a THREE-node
     * segment -- where two peers are already broadcasting when the third comes up
     * -- to expose it, and it presented as "two nodes are deaf and the third is
     * fine", which is exactly what a stalled queue looks like.
     */
    if (off == R_RBMR && (val & RBMR_EN)) {
        netc_backing_write(s, off, val, size);
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        return;
    }

    /*
     * The guest freed descriptors. Anything QEMU stalled because our ring was FULL
     * can move now -- and if we do not say so, QEMU never retries on its own. This
     * is the same lesson as the flush above, on the other end of the ring: a
     * can_receive() that has ever returned false MUST be paired with a flush at
     * every point where it could become true again, or the queue stays stalled
     * forever and the node goes silently deaf.
     */
    if (off == R_RBCIR) {
        netc_backing_write(s, off, val, size);
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        return;
    }

    switch (off) {
    case R_EMDIO_CTL:
        /* Latch the addressed PHY register for the following DATA access. */
        s->mdio_reg = val & EMDIO_CTL_DEV_MASK;
        netc_backing_write(s, off, val, size);
        return;
    case R_EMDIO_DATA:
        /* MDIO write: echo into the PHY scratch (read-back for non-special regs). */
        s->phy_regs[s->mdio_reg & 0x1F] = val & 0xFFFF;
        netc_backing_write(s, off, val, size);
        return;
    case R_TBPIR:
        /* TX producer index written -> run the internal TX->RX loopback. */
        netc_backing_write(s, off, val, size);
        netc_do_tx(s);
        return;
    default:
        netc_backing_write(s, off, val, size);
        return;
    }
}

static const MemoryRegionOps netc_ops = {
    .read = netc_read,
    .write = netc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * RESET VALUES from the RM's cold-POR column.  Offsets are ABSOLUTE within the NETC
 * window (base 0x6000_0000), matching the flat `backing` array.
 *
 * MOST OF THE IERB SET ARE **CAPABILITY** REGISTERS -- CAPR0..3, CMCAPR, IPFTMCAPR,
 * TGSMCAPR, and one L*CAPR per link.  They tell the driver how many SIs, how many
 * BD rings, how deep the tables are.  WE WERE RETURNING ZERO FROM ALL OF THEM.
 *
 * 91emulator's rule, and this is the safe side of it:
 *   "ON A CAPABILITY REGISTER, UNDER-REPORTING IS THE SAFE ERROR DIRECTION.
 *    OVER-REPORTING IS A PROMISE THE EMULATOR MAKES ON THE CHIP'S BEHALF."
 * Zero is under-reporting -- a NIC claiming no rings and no station interfaces --
 * so it fails loudly rather than shipping.  But it is still a lie, and the manual
 * hands us the truth, so there is no reason to keep telling it.
 *
 * The L*BCR link-bandwidth registers matter for a different reason: the driver
 * READ-MODIFY-WRITES them, so our zero would be laundered into its own config.
 */
static const struct { hwaddr off; uint32_t val; } netc_por[] = {
    /* NETC_IERB -- capability + config */
    { 0x800000, 0x01110651 },  /* CAPR0      */
    { 0x800004, 0x000E000E },  /* CAPR1      */
    { 0x800008, 0x00000034 },  /* CAPR2      */
    { 0x80000C, 0x00080008 },  /* CAPR3      */
    { 0x800020, 0x00002800 },  /* CMCAPR     */
    { 0x800030, 0x000000C4 },  /* IPFTMCAPR  */
    { 0x800044, 0x00000700 },  /* TGSMCAPR   */
    { 0x800080, 0x00000040 },  /* SMDTR      */
    { 0x800100, 0x06400200 },  /* HBTMAR     */
    { 0x800104, 0x00000032 },  /* HBTCR      */
    { 0x800170, 0x00000634 },  /* NETCFLRCR  */
    { 0x800178, 0x2AAAAAAA },  /* NETCCLKFR  */
    { 0x80017C, 0x000400F0 },  /* NETCCLKCR  */
    { 0x800180, 0x0000000A },  /* SBCR       */
    { 0x800190, 0x00000014 },  /* SGLTTR     */
    { 0x800300, 0x80000100 },  /* EMDIOBCR   */
    { 0x800350, 0x00000010 },  /* EMDIO_CFG  */
    /* per-link capability / bandwidth (L0..L5) */
    { 0x801000, 0x37077000 },  /* L0CAPR     */
    { 0x801014, 0x00000200 },  /* L0TXBCCTR  */
    { 0x801040, 0x37077000 },  /* L1CAPR     */
    { 0x801050, 0x00000001 },  /* L1BCR      */
    { 0x801054, 0x00000200 },  /* L1TXBCCTR  */
    { 0x801080, 0x37077000 },  /* L2CAPR     */
    { 0x801090, 0x00000002 },  /* L2BCR      */
    { 0x801094, 0x00000200 },  /* L2TXBCCTR  */
    { 0x8010C0, 0x37077000 },  /* L3CAPR     */
    { 0x8010D0, 0x00000003 },  /* L3BCR      */
    { 0x8010D4, 0x00000200 },  /* L3TXBCCTR  */
    { 0x801100, 0x37077000 },  /* L4CAPR     */
    { 0x801110, 0x00000040 },  /* L4BCR      */
    { 0x801114, 0x00000200 },  /* L4TXBCCTR  */
    { 0x801140, 0x37077010 },  /* L5CAPR     */
    { 0x801150, 0x00040041 },  /* L5BCR      */
    { 0x801154, 0x00000200 },  /* L5TXBCCTR  */
    /* PCI SR-IOV capability header, one per function F0..F4 (stride 0x1000) */
    { 0x000150, 0x00010010 },  /* NETC_F0 PCIE_CFC_SRIOV_CAP_HDR */
    { 0x001150, 0x00010010 },  /* NETC_F1 */
    { 0x002150, 0x00010010 },  /* NETC_F2 */
    { 0x003150, 0x00010010 },  /* NETC_F3 */
    { 0x004150, 0x00010010 },  /* NETC_F4 */
};

static void netc_reset(DeviceState *dev)
{
    IMXRT1180NETCState *s = IMXRT1180_NETC(dev);
    memset(s->backing, 0, IMXRT1180_NETC_SIZE);
    for (size_t i = 0; i < ARRAY_SIZE(netc_por); i++) {
        netc_backing_write(s, netc_por[i].off, netc_por[i].val, 4);
    }
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    s->mdio_reg = 0;
    memset(s->fdb, 0, sizeof(s->fdb));
    s->fdb_next_id = 0;
    memset(s->vlan, 0, sizeof(s->vlan));
    s->vlan_next_id = 0;
    s->ptp_t_base = 0;
    s->ptp_cnt_base = 0;
    s->ptp_nominal_addend = 0;
    s->ptp_cur_hi_latch = 0;
    s->ptp_enabled = false;
}

static void netc_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180NETCState *s = IMXRT1180_NETC(dev);

    s->backing = g_malloc0(IMXRT1180_NETC_SIZE);
    s->dma_as = &address_space_memory;
    memory_region_init_io(&s->iomem, OBJECT(s), &netc_ops, s,
                          TYPE_IMXRT1180_NETC, IMXRT1180_NETC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&netc_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void netc_unrealize(DeviceState *dev)
{
    IMXRT1180NETCState *s = IMXRT1180_NETC(dev);
    g_free(s->backing);
}

static const VMStateDescription vmstate_netc_fdb = {
    .name = "imxrt1180-netc/fdb-entry",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(valid, IMXRT1180NETCFdbEntry),
        VMSTATE_BOOL(dynamic, IMXRT1180NETCFdbEntry),
        VMSTATE_UINT8_ARRAY(mac, IMXRT1180NETCFdbEntry, 6),
        VMSTATE_UINT16(fid, IMXRT1180NETCFdbEntry),
        VMSTATE_UINT32(port_bitmap, IMXRT1180NETCFdbEntry),
        VMSTATE_UINT32(cfge_flags, IMXRT1180NETCFdbEntry),
        VMSTATE_UINT32(et_eid, IMXRT1180NETCFdbEntry),
        VMSTATE_UINT32(entry_id, IMXRT1180NETCFdbEntry),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_netc_vlan = {
    .name = "imxrt1180-netc/vlan-entry",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(valid, IMXRT1180NETCVlanEntry),
        VMSTATE_UINT16(vid, IMXRT1180NETCVlanEntry),
        VMSTATE_UINT32(entry_id, IMXRT1180NETCVlanEntry),
        VMSTATE_UINT32_ARRAY(cfge, IMXRT1180NETCVlanEntry, 4),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_netc = {
    .name = TYPE_IMXRT1180_NETC,
    .version_id = 4,
    .minimum_version_id = 4,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mdio_reg, IMXRT1180NETCState),
        VMSTATE_UINT16_ARRAY(phy_regs, IMXRT1180NETCState, 32),
        VMSTATE_STRUCT_ARRAY(fdb, IMXRT1180NETCState, IMXRT1180_NETC_FDB_SIZE, 1,
                             vmstate_netc_fdb, IMXRT1180NETCFdbEntry),
        VMSTATE_UINT32(fdb_next_id, IMXRT1180NETCState),
        VMSTATE_STRUCT_ARRAY(vlan, IMXRT1180NETCState, IMXRT1180_NETC_VF_SIZE, 1,
                             vmstate_netc_vlan, IMXRT1180NETCVlanEntry),
        VMSTATE_UINT32(vlan_next_id, IMXRT1180NETCState),
        VMSTATE_INT64(ptp_t_base, IMXRT1180NETCState),
        VMSTATE_UINT64(ptp_cnt_base, IMXRT1180NETCState),
        VMSTATE_UINT64(ptp_nominal_addend, IMXRT1180NETCState),
        VMSTATE_UINT32(ptp_cur_hi_latch, IMXRT1180NETCState),
        VMSTATE_BOOL(ptp_enabled, IMXRT1180NETCState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property netc_properties[] = {
    DEFINE_NIC_PROPERTIES(IMXRT1180NETCState, conf),
};

static void netc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = netc_realize;
    dc->unrealize = netc_unrealize;
    device_class_set_legacy_reset(dc, netc_reset);
    dc->vmsd = &vmstate_netc;
    device_class_set_props(dc, netc_properties);
}

static const TypeInfo netc_types[] = {
    {
        .name = TYPE_IMXRT1180_NETC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180NETCState),
        .class_init = netc_class_init,
    },
};
DEFINE_TYPES(netc_types)
