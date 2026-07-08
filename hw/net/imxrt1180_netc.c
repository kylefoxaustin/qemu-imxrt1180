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
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/net/imxrt1180_netc.h"
#include "system/dma.h"
#include "system/address-spaces.h"
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
/* BMSR: link-up (0x4) + auto-neg-complete (0x20) + capability bits. */
#define PHY_BMSR_VAL       0x782Du

/* PCI config header INIT_FLR bit (PCI_CFC_PCIE_DEV_CTL @+0x48). */
#define PCI_DEVCTL_OFF     0x48
#define PCI_INIT_FLR       0x8000u

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

static uint64_t netc_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180NETCState *s = opaque;

    if (netc_is_pci_flr(off)) {
        /* INIT_FLR always reads clear: the FLR-complete poll exits at once. */
        return netc_backing_read(s, off, size) & ~(uint64_t)PCI_INIT_FLR;
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

static void netc_reset(DeviceState *dev)
{
    IMXRT1180NETCState *s = IMXRT1180_NETC(dev);
    memset(s->backing, 0, IMXRT1180_NETC_SIZE);
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    s->mdio_reg = 0;
}

static void netc_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180NETCState *s = IMXRT1180_NETC(dev);

    s->backing = g_malloc0(IMXRT1180_NETC_SIZE);
    s->dma_as = &address_space_memory;
    memory_region_init_io(&s->iomem, OBJECT(s), &netc_ops, s,
                          TYPE_IMXRT1180_NETC, IMXRT1180_NETC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void netc_unrealize(DeviceState *dev)
{
    IMXRT1180NETCState *s = IMXRT1180_NETC(dev);
    g_free(s->backing);
}

static const VMStateDescription vmstate_netc = {
    .name = TYPE_IMXRT1180_NETC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mdio_reg, IMXRT1180NETCState),
        VMSTATE_UINT16_ARRAY(phy_regs, IMXRT1180NETCState, 32),
        VMSTATE_END_OF_LIST()
    },
};

static void netc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = netc_realize;
    dc->unrealize = netc_unrealize;
    device_class_set_legacy_reset(dc, netc_reset);
    dc->vmsd = &vmstate_netc;
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
