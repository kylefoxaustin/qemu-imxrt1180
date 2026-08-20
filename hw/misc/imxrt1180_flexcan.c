/*
 * NXP MCX N FlexCAN (Flexible Controller Area Network, CAN FD) — bring-up model.
 *
 * The bring-up blockers are the FlexCAN mode handshakes in MCR (Module
 * Configuration):
 *
 *   - On reset the module is disabled and not ready: MDIS=1, FRZ=1, HALT=1,
 *     NOTRDY=1, FRZACK=1 (RM: "When the module is enabled (MDIS becomes 0),
 *     FlexCAN automatically enters Freeze mode" with HALT/FRZ/FRZACK/NOTRDY=1).
 *   - When firmware enters Freeze mode (FRZ=1 and HALT=1, or MDIS=1) the model
 *     asserts FRZACK / LPMACK and NOTRDY so the "wait for FRZACK" / "wait for
 *     LPMACK" poll completes.
 *   - When firmware leaves Freeze and enables the module (HALT=0, FRZ cleared
 *     or MDIS=0) the model clears NOTRDY / FRZACK / LPMACK so the "wait until
 *     ready" poll completes.
 *   - MCR.SOFTRST self-clears immediately (the reset is instantaneous here).
 *
 * ESR1 (error/status) and IFLAG1 (message-buffer interrupt flags) are
 * write-1-to-clear.  ESR2, CRCR, RXFIR, FDCRC are read-only status that read
 * idle/zero.  All other registers (including the message-buffer RAM, individual
 * mask RAM and enhanced RX FIFO filter RAM) are backed permissively by regs[].
 *
 * Offsets/bits from the MCXN947 CMSIS header (CAN_Type); mode semantics from the
 * FlexCAN chapter of the reference manual.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/misc/imxrt1180_flexcan.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* Control register offsets (CAN_Type). */
#define R_MCR      0x000   /* Module Configuration */
#define R_CTRL1    0x004
#define R_TIMER    0x008
#define R_ECR      0x01C   /* Error Counter */
#define R_ESR1     0x020   /* Error and Status 1 (W1C flags) */
#define R_IMASK1   0x028
#define R_IFLAG1   0x030   /* Interrupt Flags 1 (W1C) */
#define R_CTRL2    0x034
#define R_ESR2     0x038   /* Error and Status 2 (RO) */
#define R_CRCR     0x044   /* CRC (RO) */
#define R_RXFIR    0x04C   /* Legacy RX FIFO Information (RO) */
#define R_FDCRC    0xC08   /* CAN FD CRC (RO) */
#define R_RXMGMASK 0x010   /* RX Message Buffers Global Mask */

/* CTRL1 loopback-mode enable. */
#define CTRL1_LPB  (1u << 12)

/*
 * Classic message-buffer array: MB[32] at offset 0x80, step 0x10
 * (CS, ID, WORD0, WORD1).  CS holds CODE[27:24] and DLC[19:16].
 */
#define MB_BASE        0x080
#define MB_STRIDE      0x010
#define MB_COUNT       96
#define MB_CODE_SHIFT  24
#define MB_CODE_MASK   0xFu
#define CODE_RX_EMPTY  0x4   /* MB configured to receive, currently empty */
#define CODE_RX_FULL   0x2   /* MB holds a received frame */
#define CODE_TX_DATA   0xC   /* MB armed to transmit a data frame */

/* CS control bits (classic frame): SRR[22], IDE[21], RTR[20], DLC[19:16]. */
#define MB_SRR    (1u << 22)
#define MB_IDE    (1u << 21)
#define MB_RTR    (1u << 20)
#define MB_DLC(cs)  (((cs) >> 16) & 0xF)

/* MCR bit positions (CAN_MCR_*_SHIFT). */
#define MCR_LPMACK   (1u << 20)
#define MCR_FRZACK   (1u << 24)
#define MCR_SOFTRST  (1u << 25)
#define MCR_NOTRDY   (1u << 27)
#define MCR_HALT     (1u << 28)
#define MCR_FRZ      (1u << 30)
#define MCR_MDIS     (1u << 31)

/* MAXMB (CAN_MCR_MAXMB) occupies bits [6:0]; reset value is 0x0F. */
#define MCR_RESET    (MCR_MDIS | MCR_FRZ | MCR_HALT | MCR_NOTRDY | MCR_FRZACK | \
                      0x0000000Fu)

/*
 * Recompute the MCR acknowledge/ready bits from the freeze/disable request
 * bits.  Freeze is requested by (FRZ and HALT) or by MDIS (low-power).  In
 * either case the module is not ready and acknowledges; otherwise it is ready.
 */
static uint32_t flexcan_mcr_settle(uint32_t mcr)
{
    bool freeze = (mcr & MCR_FRZ) && (mcr & MCR_HALT);
    bool disable = mcr & MCR_MDIS;

    mcr &= ~(MCR_FRZACK | MCR_LPMACK | MCR_NOTRDY);
    if (freeze) {
        mcr |= MCR_FRZACK | MCR_NOTRDY;
    }
    if (disable) {
        mcr |= MCR_LPMACK | MCR_NOTRDY;
    }
    return mcr;
}

static void flexcan_update_irq(IMXRT1180FlexCanState *s)
{
    bool active = (s->regs[R_IFLAG1 >> 2] & s->regs[R_IMASK1 >> 2]) != 0;
    qemu_set_irq(s->irq, active);
}

/*
 * Software transmit from message buffer "tx".  When the controller is in
 * loopback mode (CTRL1.LPB), the transmitted frame is delivered internally to
 * the first RX-empty message buffer whose ID matches under the global mask
 * (RXMGMASK; a 0 mask bit is "don't care", so a reset mask of 0 accepts any
 * ID).  The receiving MB is filled (CODE=FULL, ID/DLC/data copied) and its
 * IFLAG1 bit set; the transmitting MB also raises its IFLAG1 (transmit done).
 */
/* Build a classic qemu_can_frame from TX message buffer "tx" and put it on the
 * emulated CAN bus (a can-host-chardev bridges the bus to a socket peer). */
static void flexcan_send_to_bus(IMXRT1180FlexCanState *s, unsigned tx)
{
    uint32_t t = (MB_BASE + tx * MB_STRIDE) >> 2;
    uint32_t cs = s->regs[t], id = s->regs[t + 1];
    qemu_can_frame f = { 0 };
    uint8_t len = MB_DLC(cs), i;

    if (len > 8) {
        len = 8;
    }
    if (cs & MB_IDE) {
        f.can_id = (id & QEMU_CAN_EFF_MASK) | QEMU_CAN_EFF_FLAG;
    } else {
        f.can_id = (id >> 18) & QEMU_CAN_SFF_MASK;
    }
    if (cs & MB_RTR) {
        f.can_id |= QEMU_CAN_RTR_FLAG;
    }
    f.can_dlc = len;
    for (i = 0; i < len; i++) {
        f.data[i] = (s->regs[t + 2 + i / 4] >> (24 - 8 * (i % 4))) & 0xFF;
    }
    can_bus_client_send(&s->bus_client, &f, 1);
}

/* A frame arrived from the CAN bus: land it in the first RX-empty message
 * buffer (RXMGMASK=0 => any ID; the guest filters in software), set its flag. */
static ssize_t flexcan_bus_receive(CanBusClientState *client,
                                   const qemu_can_frame *frames,
                                   size_t frames_cnt)
{
    IMXRT1180FlexCanState *s = container_of(client, IMXRT1180FlexCanState, bus_client);
    const qemu_can_frame *f = frames;
    bool eff, rtr;
    unsigned rx;
    uint8_t len, i;

    if (!frames_cnt || (f->can_id & QEMU_CAN_ERR_FLAG)) {
        return frames_cnt;
    }
    eff = f->can_id & QEMU_CAN_EFF_FLAG;
    rtr = f->can_id & QEMU_CAN_RTR_FLAG;
    len = f->can_dlc > 8 ? 8 : f->can_dlc;

    for (rx = 0; rx < MB_COUNT; rx++) {
        uint32_t r = (MB_BASE + rx * MB_STRIDE) >> 2;

        if (((s->regs[r] >> MB_CODE_SHIFT) & MB_CODE_MASK) != CODE_RX_EMPTY) {
            continue;
        }
        s->regs[r + 1] = eff ? (f->can_id & QEMU_CAN_EFF_MASK)
                             : ((f->can_id & QEMU_CAN_SFF_MASK) << 18);
        s->regs[r + 2] = s->regs[r + 3] = 0;
        for (i = 0; i < len; i++) {
            s->regs[r + 2 + i / 4] |= (uint32_t)f->data[i] << (24 - 8 * (i % 4));
        }
        s->regs[r] = (CODE_RX_FULL << MB_CODE_SHIFT) | ((uint32_t)len << 16) |
                     (eff ? (MB_IDE | MB_SRR) : 0) | (rtr ? MB_RTR : 0);
        s->regs[R_IFLAG1 >> 2] |= (1u << rx);
        flexcan_update_irq(s);
        return 1;
    }
    return 1;   /* no free RX MB: drop (a real device flags overrun) */
}

static bool flexcan_bus_can_receive(CanBusClientState *client)
{
    return true;
}

static CanBusClientInfo flexcan_bus_client_info = {
    .can_receive = flexcan_bus_can_receive,
    .receive     = flexcan_bus_receive,
};

static void flexcan_transmit(IMXRT1180FlexCanState *s, unsigned tx)
{
    uint32_t t = (MB_BASE + tx * MB_STRIDE) >> 2;
    uint32_t tcs = s->regs[t];
    uint32_t tid = s->regs[t + 1];

    if (s->canbus && !(s->regs[R_CTRL1 >> 2] & CTRL1_LPB)) {
        /* Board-to-board: put the frame on the real CAN bus. */
        flexcan_send_to_bus(s, tx);
    } else if (s->regs[R_CTRL1 >> 2] & CTRL1_LPB) {
        uint32_t mask = s->regs[R_RXMGMASK >> 2];
        unsigned rx;

        for (rx = 0; rx < MB_COUNT; rx++) {
            uint32_t r = (MB_BASE + rx * MB_STRIDE) >> 2;
            uint32_t rcs = s->regs[r];

            if (rx == tx) {
                continue;
            }
            if (((rcs >> MB_CODE_SHIFT) & MB_CODE_MASK) != CODE_RX_EMPTY) {
                continue;
            }
            if (((tid ^ s->regs[r + 1]) & mask) != 0) {
                continue;
            }
            /* Deliver the frame: keep DLC/RTR/IDE/SRR, set CODE=FULL. */
            s->regs[r]     = (CODE_RX_FULL << MB_CODE_SHIFT) | (tcs & 0x00FF0000u);
            s->regs[r + 1] = tid;
            s->regs[r + 2] = s->regs[t + 2];
            s->regs[r + 3] = s->regs[t + 3];
            s->regs[R_IFLAG1 >> 2] |= (1u << rx);
            break;
        }
    }

    /* Transmit complete: the TX message buffer raises its own interrupt. */
    s->regs[R_IFLAG1 >> 2] |= (1u << tx);
    flexcan_update_irq(s);
}

static uint64_t flexcan_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180FlexCanState *s = IMXRT1180_FLEXCAN(opaque);
    uint32_t shift = (off & 3) * 8;
    uint32_t idx = off >> 2;
    uint32_t v;

    if (off >= IMXRT1180_FLEXCAN_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }

    switch (off & ~3u) {
    case R_ESR2:
    case R_CRCR:
    case R_RXFIR:
    case R_FDCRC:
        /* Read-only status: idle. */
        v = 0;
        break;
    default:
        v = s->regs[idx];
        break;
    }

    /* Support byte/halfword reads by shifting the backing word. */
    return (v >> shift) & ((size == 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1));
}

static void flexcan_write(void *opaque, hwaddr off, uint64_t value,
                          unsigned size)
{
    IMXRT1180FlexCanState *s = IMXRT1180_FLEXCAN(opaque);
    uint32_t idx = off >> 2;
    uint32_t shift = (off & 3) * 8;
    uint32_t mask = (size == 4) ? 0xFFFFFFFFu : (((1u << (size * 8)) - 1) << shift);
    uint32_t val;

    if (off >= IMXRT1180_FLEXCAN_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    /* Merge sub-word writes into the 32-bit register value. */
    val = (s->regs[idx] & ~mask) | ((uint32_t)(value << shift) & mask);

    switch (off & ~3u) {
    case R_MCR:
        /* SOFTRST self-clears: the reset is instantaneous in the model. */
        val &= ~MCR_SOFTRST;
        s->regs[idx] = flexcan_mcr_settle(val);
        return;
    case R_ESR1:
        /* Write-1-to-clear status flags. */
        s->regs[idx] &= ~((uint32_t)(value << shift) & mask);
        return;
    case R_IFLAG1:
        /* Write-1-to-clear MB interrupt flags; re-evaluate the IRQ. */
        s->regs[idx] &= ~((uint32_t)(value << shift) & mask);
        flexcan_update_irq(s);
        return;
    case R_IMASK1:
        s->regs[idx] = val;
        flexcan_update_irq(s);
        return;
    case R_ESR2:
    case R_CRCR:
    case R_RXFIR:
    case R_FDCRC:
        /* Read-only status: ignore writes. */
        return;
    default:
        s->regs[idx] = val;
        /* Arming a message buffer for transmit (CS CODE=TX_DATA) sends it. */
        if (off >= MB_BASE && off < MB_BASE + MB_COUNT * MB_STRIDE &&
            ((off - MB_BASE) % MB_STRIDE) == 0 &&
            ((val >> MB_CODE_SHIFT) & MB_CODE_MASK) == CODE_TX_DATA) {
            flexcan_transmit(s, (off - MB_BASE) / MB_STRIDE);
        }
        return;
    }
}

static const MemoryRegionOps flexcan_ops = {
    .read = flexcan_read,
    .write = flexcan_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void imxrt1180_flexcan_reset(DeviceState *dev)
{
    IMXRT1180FlexCanState *s = IMXRT1180_FLEXCAN(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_MCR >> 2] = MCR_RESET;
}

static void imxrt1180_flexcan_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180FlexCanState *s = IMXRT1180_FLEXCAN(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &flexcan_ops, s,
                          TYPE_IMXRT1180_FLEXCAN, IMXRT1180_FLEXCAN_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    /* Board-to-board CAN: join the emulated bus if one was linked. */
    if (s->canbus) {
        s->bus_client.info = &flexcan_bus_client_info;
        if (can_bus_insert_client(s->canbus, &s->bus_client) < 0) {
            error_setg(errp, "failed to join CAN bus");
            return;
        }
    }
}

/* Migration: re-derive the IRQ line level from restored register state --
 * qemu_irq output levels are not migrated, so a pending IRQ would be lost. */
static int vmstate_imxrt1180_flexcan_post_load(void *opaque, int version_id)
{
    flexcan_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_imxrt1180_flexcan = {
    .name = TYPE_IMXRT1180_FLEXCAN,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = vmstate_imxrt1180_flexcan_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180FlexCanState, IMXRT1180_FLEXCAN_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imxrt1180_flexcan_properties[] = {
    DEFINE_PROP_LINK("canbus", IMXRT1180FlexCanState, canbus, TYPE_CAN_BUS,
                     CanBusState *),
};

static void imxrt1180_flexcan_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_flexcan_realize;
    device_class_set_legacy_reset(dc, imxrt1180_flexcan_reset);
    dc->vmsd = &vmstate_imxrt1180_flexcan;
    device_class_set_props(dc, imxrt1180_flexcan_properties);
}

static const TypeInfo imxrt1180_flexcan_types[] = {
    {
        .name          = TYPE_IMXRT1180_FLEXCAN,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180FlexCanState),
        .class_init    = imxrt1180_flexcan_class_init,
    },
};

DEFINE_TYPES(imxrt1180_flexcan_types)
