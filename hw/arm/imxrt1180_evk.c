/*
 * NXP MIMXRT1180-EVK reference board
 *
 * Thin board layer: provide the source clocks, instantiate the i.MX RT1180 SoC
 * with the chosen part (MIMXRT1189), and load the firmware image.  Everything
 * device-specific lives in the SoC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/arm/boot.h"
#include "hw/arm/imxrt1180_soc.h"
#include "hw/arm/machines-qom.h"   /* DEFINE_MACHINE_ARM / arm_machine_interfaces */
#include "qom/object.h"

/* CM33 boot core runs at 300 MHz on the RT1189 (M7 main core is 800 MHz). */
#define IMXRT1180_M33_SYSCLK_HZ  300000000ULL

static void mimxrt1180_evk_init(MachineState *machine)
{
    IMXRT1180State *soc;
    Clock          *sysclk, *refclk;
    DeviceState    *dev;

    /* Board-supplied source clocks. */
    sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, IMXRT1180_M33_SYSCLK_HZ);
    refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(refclk, IMXRT1180_M33_SYSCLK_HZ);

    /* Instantiate the SoC. */
    soc = IMXRT1180_SOC(object_new(TYPE_IMXRT1180_SOC));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    object_unref(OBJECT(soc));

    dev = DEVICE(soc);
    qdev_prop_set_string(dev, "part", "MIMXRT1189");
    qdev_connect_clock_in(dev, "sysclk", sysclk);
    qdev_connect_clock_in(dev, "refclk", refclk);
    sysbus_realize(SYS_BUS_DEVICE(soc), &error_fatal);

    /* Load the firmware (ELF or raw .bin) into the code TCM, where the SDK's
     * default CM33 debug/RAM images link their vector table; the reset vector
     * is taken from there (init-svtor == code TCM base). */
    armv7m_load_kernel(ARM_CPU(first_cpu),
                       machine->kernel_filename,
                       IMXRT1180_CODE_TCM_BASE,
                       IMXRT1180_CODE_TCM_SIZE);
}

static void mimxrt1180_evk_machine_init(MachineClass *mc)
{
    mc->desc        = "NXP MIMXRT1180-EVK (i.MX RT1189, Cortex-M33 + Cortex-M7)";
    mc->init        = mimxrt1180_evk_init;
    mc->default_ram_size = 0;   /* SoC owns its memories */
    /* Heterogeneous dual-core: cpu0 = M33 (boots), cpu1 = M7 (released by the
     * M33 via SRC/BLK_CTRL).  Lock the count so TCG provisions both contexts. */
    mc->default_cpus = 2;
    mc->min_cpus     = 2;
    mc->max_cpus     = 2;
    /* Keep transaction failures visible so peripheral stubs are loud. */
    mc->ignore_memory_transaction_failures = false;
}

DEFINE_MACHINE_ARM("mimxrt1180-evk", mimxrt1180_evk_machine_init)
