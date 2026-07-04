/*
 * NXP i.MX RT1180 RGPIO (Rapid GPIO).
 *
 * Functional GPIO: PDOR/PSOR/PCOR/PTOR drive the output latch, PDDR selects
 * direction, PDIR reads the pin level, and each output pin is exposed as a
 * qemu_irq so a board/test can observe it (e.g. the EVK user LED on RGPIO4[27],
 * which led_blinky toggles).
 *
 * Register offsets verified against the MIMXRT1189 CMSIS PERI_RGPIO.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/gpio/imxrt1180_rgpio.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#define RGPIO_VERID  0x00
#define RGPIO_PARAM  0x04
#define RGPIO_PDOR   0x40
#define RGPIO_PSOR   0x44
#define RGPIO_PCOR   0x48
#define RGPIO_PTOR   0x4C
#define RGPIO_PDIR   0x50
#define RGPIO_PDDR   0x54
#define RGPIO_PIDR   0x58

/* Pin level seen at PDIR: driven value on output pins, external level on
 * inputs (unless input is disabled). */
static uint32_t rgpio_pin_level(IMXRT1180RGPIOState *s)
{
    return (s->pdor & s->pddr) | (s->in & ~s->pddr & ~s->idr);
}

static void rgpio_update_outputs(IMXRT1180RGPIOState *s, uint32_t old_driven)
{
    uint32_t driven = s->pdor & s->pddr;
    uint32_t changed = driven ^ old_driven;

    for (int i = 0; i < IMXRT1180_RGPIO_PINS; i++) {
        if (changed & (1u << i)) {
            qemu_set_irq(s->output[i], (driven >> i) & 1);
        }
    }
}

static uint64_t imxrt1180_rgpio_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180RGPIOState *s = IMXRT1180_RGPIO(opaque);

    switch (offset) {
    case RGPIO_VERID: return 0x02000000;                 /* plausible version */
    case RGPIO_PARAM: return IMXRT1180_RGPIO_PINS;       /* pin count         */
    case RGPIO_PDOR:  return s->pdor;
    case RGPIO_PDIR:  return rgpio_pin_level(s);
    case RGPIO_PDDR:  return s->pddr;
    case RGPIO_PIDR:  return s->idr;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void imxrt1180_rgpio_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    IMXRT1180RGPIOState *s = IMXRT1180_RGPIO(opaque);
    uint32_t old = s->pdor & s->pddr;

    switch (offset) {
    case RGPIO_PDOR: s->pdor = value; break;
    case RGPIO_PSOR: s->pdor |= value; break;
    case RGPIO_PCOR: s->pdor &= ~(uint32_t)value; break;
    case RGPIO_PTOR: s->pdor ^= value; break;
    case RGPIO_PDDR: s->pddr = value; break;
    case RGPIO_PIDR: s->idr = value; break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled write @0x%" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, (uint32_t)value);
        return;
    }
    rgpio_update_outputs(s, old);
}

static const MemoryRegionOps imxrt1180_rgpio_ops = {
    .read = imxrt1180_rgpio_read,
    .write = imxrt1180_rgpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_rgpio_reset(DeviceState *dev)
{
    IMXRT1180RGPIOState *s = IMXRT1180_RGPIO(dev);

    s->pdor = s->pddr = s->idr = s->in = 0;
}

static void imxrt1180_rgpio_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180RGPIOState *s = IMXRT1180_RGPIO(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_rgpio_ops, s,
                          TYPE_IMXRT1180_RGPIO, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    qdev_init_gpio_out(dev, s->output, IMXRT1180_RGPIO_PINS);
}

static const VMStateDescription vmstate_imxrt1180_rgpio = {
    .name = TYPE_IMXRT1180_RGPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pdor, IMXRT1180RGPIOState),
        VMSTATE_UINT32(pddr, IMXRT1180RGPIOState),
        VMSTATE_UINT32(idr, IMXRT1180RGPIOState),
        VMSTATE_UINT32(in, IMXRT1180RGPIOState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_rgpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_rgpio_realize;
    device_class_set_legacy_reset(dc, imxrt1180_rgpio_reset);
    dc->vmsd = &vmstate_imxrt1180_rgpio;
}

static const TypeInfo imxrt1180_rgpio_types[] = {
    {
        .name          = TYPE_IMXRT1180_RGPIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180RGPIOState),
        .class_init    = imxrt1180_rgpio_class_init,
    },
};

DEFINE_TYPES(imxrt1180_rgpio_types)
