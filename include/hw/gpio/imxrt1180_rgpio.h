/*
 * NXP i.MX RT1180 RGPIO (Rapid GPIO).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_GPIO_IMXRT1180_RGPIO_H
#define HW_GPIO_IMXRT1180_RGPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_RGPIO "imxrt1180-rgpio"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180RGPIOState, IMXRT1180_RGPIO)

#define IMXRT1180_RGPIO_PINS 32

struct IMXRT1180RGPIOState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     output[IMXRT1180_RGPIO_PINS]; /* per-pin output line */

    uint32_t pdor;   /* port data output   */
    uint32_t pddr;   /* port data direction (1 = output) */
    uint32_t idr;    /* input disable      */
    uint32_t in;     /* externally-driven input level */
};

#endif /* HW_GPIO_IMXRT1180_RGPIO_H */
