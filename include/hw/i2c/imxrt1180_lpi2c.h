/*
 * NXP i.MX RT1180 LPI2C — Low-Power I2C, controller (master) mode.
 *
 * Command-FIFO master: firmware pushes commands to MTDR (CMD[10:8], DATA[7:0])
 * — START+address, transmit, receive-N, STOP — and reads bytes back from MRDR.
 * Modelled on a real QEMU I2CBus so ordinary i2c device models attach to it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_I2C_IMXRT1180_LPI2C_H
#define HW_I2C_IMXRT1180_LPI2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_LPI2C "imxrt1180-lpi2c"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180LPI2CState, IMXRT1180_LPI2C)

#define IMXRT1180_LPI2C_FIFO 16   /* rx FIFO depth (>= real, synchronous model) */

struct IMXRT1180LPI2CState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;

    uint32_t mcr;                 /* Control */
    uint32_t msr_sticky;          /* W1C status bits (SDF/NDF/... ) */
    uint32_t mier;                /* Interrupt Enable */
    uint32_t mder;                /* DMA Enable */
    uint32_t mcfgr[4];            /* Config 0..3 */
    uint32_t mdmr;                /* Data Match */
    uint32_t mccr[2];             /* Clock Config 0/1 */
    uint32_t mfcr;               /* FIFO Control (watermarks) */

    uint8_t  rx_fifo[IMXRT1180_LPI2C_FIFO];
    uint8_t  rx_head;
    uint8_t  rx_count;
    bool     active;              /* a transfer is in progress (MBF) */
};

#endif /* HW_I2C_IMXRT1180_LPI2C_H */
