/*
 * NXP i.MX RT1180 FlexCAN (Flexible Controller Area Network, CAN FD) — bring-up model.
 *
 * Models the FlexCAN control registers accurately enough that firmware's
 * module-disable / freeze / soft-reset init handshakes settle and the init loop
 * terminates.  The full mapped window (control registers, message-buffer RAM,
 * individual mask RAM, enhanced RX FIFO filter RAM) is backed by a flat regs[]
 * array.  One QOM type serves both CAN1..3.  Offsets/bits from the
 * MIMXRT1189 CMSIS header (CAN_Type); semantics from the FlexCAN chapter of the RM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_FLEXCAN_H
#define HW_MISC_IMXRT1180_FLEXCAN_H

#include "hw/core/sysbus.h"
#include "net/can_emu.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_FLEXCAN "imxrt1180-flexcan"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180FlexCanState, IMXRT1180_FLEXCAN)

/*
 * CAN_Type spans control registers through the enhanced RX FIFO filter array
 * (ERFFEL[32] at offset 0x3000, ending at 0x3080).  CAN1..3 are spaced
 * 0x4000 apart in the memory map, so the mapped window is 0x4000.
 */
#define IMXRT1180_FLEXCAN_SIZE 0x4000

struct IMXRT1180FlexCanState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMXRT1180_FLEXCAN_SIZE / 4];

    /* Board-to-board CAN: when `canbus` is linked, TX message buffers put
     * frames onto the emulated CAN bus (a can-host-chardev then bridges it to a
     * socket) and bus frames land in RX message buffers.  NULL = loopback-only. */
    CanBusState       *canbus;
    CanBusClientState  bus_client;
};

#endif /* HW_MISC_IMXRT1180_FLEXCAN_H */
