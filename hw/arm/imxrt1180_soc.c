/*
 * NXP i.MX RT1180 crossover MCU SoC (Arm Cortex-M33 + Cortex-M7)
 *
 * MVP scaffold: the secure Cortex-M33 boot core, the on-chip memory map, and a
 * catch-all "unimplemented" peripheral window.  Peripherals (console LPUART,
 * clocks, ...) and the second core (Cortex-M7) are layered on top per the
 * -d unimp,guest_errors bring-up loop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/imxrt1180_soc.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h" /* qdev_prop_set_chr */
#include "hw/core/qdev-clock.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"   /* get_system_memory() */
#include "system/system.h"           /* serial_hd() */

/*
 * Per-SKU table.  MIMXRT1189 is the fully-loaded family part (== the silicon on
 * the MIMXRT1180-EVK, MIMXRT1189CVM8B).  Values VERIFIED against the MIMXRT1189
 * CMSIS headers: NVIC has 239 external lines (highest ECAT_RST_OUT_IRQn = 238);
 * __NVIC_PRIO_BITS is 3 on the M33 and 4 on the M7.
 */
static const IMXRT1180Config imxrt1180_configs[] = {
    {
        .name     = "MIMXRT1189",
        .num_cpus = 2,   /* M33 boot core + M7 (held off, released by the M33) */
        .num_irq  = 239,
        .core     = {
            [IMXRT1180_CPU_M33] = { ARM_CPU_TYPE_NAME("cortex-m33"), 3 },
            [IMXRT1180_CPU_M7]  = { ARM_CPU_TYPE_NAME("cortex-m7"),  4 },
        },
    },
};

static const IMXRT1180Config *imxrt1180_lookup(const char *part)
{
    for (size_t i = 0; i < ARRAY_SIZE(imxrt1180_configs); i++) {
        if (!strcmp(part, imxrt1180_configs[i].name)) {
            return &imxrt1180_configs[i];
        }
    }
    return NULL;
}

static void imxrt1180_soc_instance_init(Object *obj)
{
    IMXRT1180State *s = IMXRT1180_SOC(obj);

    /* Cores are initialized in realize() once the part (and thus the active
     * core count) is known — only the cores we actually realize are created,
     * so no half-built child trips qdev's realized-properly assert. */
    object_initialize_child(obj, "lpuart1", &s->lpuart1, TYPE_IMXRT1180_LPUART);
    object_initialize_child(obj, "anadig", &s->anadig, TYPE_IMXRT1180_ANADIG);
    for (int i = 0; i < IMXRT1180_NUM_RTWDOG; i++) {
        g_autofree char *name = g_strdup_printf("rtwdog%d", i + 1);
        object_initialize_child(obj, name, &s->rtwdog[i], TYPE_IMXRT1180_RTWDOG);
    }
    object_initialize_child(obj, "mu-rt-s3", &s->mu_rt_s3, TYPE_IMXRT1180_S3MU);
    object_initialize_child(obj, "flexspi1", &s->flexspi1_ctrl, TYPE_IMXRT1180_FLEXSPI);
    object_initialize_child(obj, "ccm", &s->ccm, TYPE_IMXRT1180_CCM);
    object_initialize_child(obj, "flexspi2", &s->flexspi2_ctrl, TYPE_IMXRT1180_FLEXSPI);
    for (int i = 0; i < IMXRT1180_NUM_RGPIO; i++) {
        g_autofree char *rname = g_strdup_printf("rgpio%d", i + 1);
        object_initialize_child(obj, rname, &s->rgpio[i], TYPE_IMXRT1180_RGPIO);
    }
    object_initialize_child(obj, "src", &s->src, TYPE_IMXRT1180_SRC);
    for (int i = 0; i < IMXRT1180_NUM_TRDC; i++) {
        g_autofree char *tname = g_strdup_printf("trdc%d", i + 1);
        object_initialize_child(obj, tname, &s->trdc[i], TYPE_IMXRT1180_TRDC);
    }

    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
    s->refclk = qdev_init_clock_in(DEVICE(s), "refclk", NULL, NULL, 0);
}

static void imxrt1180_soc_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180State *s             = IMXRT1180_SOC(dev);
    MemoryRegion   *system_memory = get_system_memory();
    const IMXRT1180Config *cfg;
    uint32_t ncpu;

    cfg = imxrt1180_lookup(s->part ? s->part : "MIMXRT1189");
    if (!cfg) {
        error_setg(errp, "imxrt1180-soc: unknown part '%s'", s->part);
        return;
    }
    s->cfg = cfg;

    /* --- On-chip memories (MIMXRT1189, CM33 view) ------------------------- *
     * RAM-backed during bring-up: the -kernel loader (and later the FlexSPI
     * NOR / FMU path) writes them.  TrustZone-M secure aliases are added when
     * the security model is layered on.
     */
    memory_region_init_ram(&s->code_tcm, OBJECT(dev), "imxrt1180.code-tcm",
                           IMXRT1180_CODE_TCM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, IMXRT1180_CODE_TCM_BASE,
                                &s->code_tcm);

    memory_region_init_ram(&s->sys_tcm, OBJECT(dev), "imxrt1180.sys-tcm",
                           IMXRT1180_SYS_TCM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, IMXRT1180_SYS_TCM_BASE,
                                &s->sys_tcm);

    memory_region_init_ram(&s->ocram1, OBJECT(dev), "imxrt1180.ocram1",
                           IMXRT1180_OCRAM1_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, IMXRT1180_OCRAM1_BASE,
                                &s->ocram1);

    memory_region_init_ram(&s->ocram2, OBJECT(dev), "imxrt1180.ocram2",
                           IMXRT1180_OCRAM2_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, IMXRT1180_OCRAM2_BASE,
                                &s->ocram2);

    memory_region_init_ram(&s->flexspi1, OBJECT(dev), "imxrt1180.flexspi1-nor",
                           IMXRT1180_FLEXSPI1_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, IMXRT1180_FLEXSPI1_BASE,
                                &s->flexspi1);

    /* --- CPU cores + NVIC + SysTick -------------------------------------- *
     * Each ARMV7M wraps its "memory" link in its own private per-core
     * container (with that core's NVIC/SysTick/PPB), so cores cannot share a
     * single MemoryRegion directly — each gets its OWN alias of the shared SoC
     * map.  cpu0 (M33) is the boot core; cpu1 (M7) is released by the M33 and
     * is wired in a later step (num_cpus == 1 for the MVP).
     */
    ncpu = cfg->num_cpus ? cfg->num_cpus : 1;
    if (ncpu > IMXRT1180_MAX_CPUS) {
        ncpu = IMXRT1180_MAX_CPUS;
    }
    for (uint32_t i = 0; i < ncpu; i++) {
        g_autofree char *cname = g_strdup_printf("cpu%d", i);
        g_autofree char *view  = g_strdup_printf("imxrt1180-cpu%d-view", i);
        DeviceState *cpudev;

        object_initialize_child(OBJECT(dev), cname, &s->armv7m[i], TYPE_ARMV7M);
        cpudev = DEVICE(&s->armv7m[i]);

        memory_region_init_alias(&s->cpu_mem[i], OBJECT(dev), view,
                                 system_memory, 0, UINT64_MAX);

        qdev_prop_set_uint32(cpudev, "num-irq",       cfg->num_irq);
        qdev_prop_set_uint8 (cpudev, "num-prio-bits", cfg->core[i].num_prio_bits);
        qdev_prop_set_string(cpudev, "cpu-type",      cfg->core[i].cpu_type);
        qdev_prop_set_bit   (cpudev, "enable-bitband", false);
        /*
         * Reset reads the vector table (initial SP + reset PC) from init-svtor.
         * The SDK's default CM33 debug/RAM images link their vector table to
         * the code TCM (0x0FFE0000); FlexSPI-NOR XIP boot (0x28000000) needs the
         * boot-ROM container parse and is a follow-on.  Point reset at code TCM
         * so the stock SDK images boot directly.
         */
        qdev_prop_set_uint32(cpudev, "init-svtor",    IMXRT1180_CODE_TCM_BASE);
        if (i > 0) {
            qdev_prop_set_bit(cpudev, "start-powered-off", true);
        }
        qdev_connect_clock_in(cpudev, "cpuclk", s->sysclk);
        qdev_connect_clock_in(cpudev, "refclk", s->refclk);
        object_property_set_link(OBJECT(&s->armv7m[i]), "memory",
                                 OBJECT(&s->cpu_mem[i]), &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m[i]), errp)) {
            return;
        }
    }

    /* --- Peripherals: catch-all over both TZ-M / power-domain aliases ----- *
     * Real models are mapped AFTER the catch-all so they override it by
     * memory-region priority; run with -d unimp,guest_errors to drive the
     * bring-up order.
     */
    create_unimplemented_device("imxrt1180.periph", IMXRT1180_PERIPH_BASE,
                                IMXRT1180_PERIPH_SIZE);

    /* LPUART1 — debug console.  Binds host serial_hd(0); IRQ 19 -> M33 NVIC. */
    qdev_prop_set_chr(DEVICE(&s->lpuart1), "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpuart1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpuart1), 0, IMXRT1180_LPUART1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpuart1), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M33]),
                                        IMXRT1180_LPUART1_IRQ));

    /* ANADIG — analog clock block (OSC/PLL); reports clocks stable/locked so
     * the SDK CLOCK_Init poll loops complete. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->anadig), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->anadig), 0, IMXRT1180_ANADIG_BASE);

    /* RTWDOG1..5 — SystemInit unlocks + disables each early in boot. */
    static const hwaddr rtwdog_base[IMXRT1180_NUM_RTWDOG] = {
        0x442D0000, /* RTWDOG1 (AON)    */
        0x442E0000, /* RTWDOG2 (AON)    */
        0x42490000, /* RTWDOG3 (WAKEUP) */
        0x424A0000, /* RTWDOG4 (WAKEUP) */
        0x424B0000, /* RTWDOG5 (WAKEUP) */
    };
    for (int i = 0; i < IMXRT1180_NUM_RTWDOG; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtwdog[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtwdog[i]), 0, rtwdog_base[i]);
    }

    /* MU to the EdgeLock secure enclave (ELE) — honest request/ack handshake. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mu_rt_s3), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mu_rt_s3), 0, IMXRT1180_MU_RT_S3MU_BASE);

    /* FlexSPI1 controller — reports idle so board-init config/poll completes. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flexspi1_ctrl), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexspi1_ctrl), 0,
                    IMXRT1180_FLEXSPI1_CTRL_BASE);

    /* CCM — clock roots/gates report ready so the SDK clock code completes. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ccm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ccm), 0, IMXRT1180_CCM_BASE);

    /* FlexSPI2 controller (same model as FlexSPI1). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flexspi2_ctrl), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexspi2_ctrl), 0,
                    IMXRT1180_FLEXSPI2_CTRL_BASE);

    /* RGPIO1..6 — functional GPIO (EVK user LED is RGPIO4[27]). */
    static const hwaddr rgpio_base[IMXRT1180_NUM_RGPIO] = {
        0x47400000, /* RGPIO1 */
        0x43810000, /* RGPIO2 */
        0x43820000, /* RGPIO3 */
        0x43830000, /* RGPIO4 */
        0x43840000, /* RGPIO5 */
        0x43850000, /* RGPIO6 */
    };
    for (int i = 0; i < IMXRT1180_NUM_RGPIO; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->rgpio[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->rgpio[i]), 0, rgpio_base[i]);
    }

    /* Cortex-M7 TCM in the system (M33) view — the M33 loads/clears the M7
     * image here; a system-view M7 boot image lives in this window. */
    memory_region_init_ram(&s->cm7_tcm, OBJECT(dev), "imxrt1180.cm7-tcm",
                           IMXRT1180_CM7_TCM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, IMXRT1180_CM7_TCM_BASE,
                                &s->cm7_tcm);

    /* SRC + BLK_CTRL_S_AONMIX — the M33 releases the M7 through these. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->src), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->src), 0, IMXRT1180_SRC_GENERAL_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->src), 1, IMXRT1180_BLK_CTRL_S_AON_BASE);
    /* Give the SRC the M7 CPU so BT_RELEASE_M7 can start it (cpu1 = M7). */
    if (ncpu > IMXRT1180_CPU_M7) {
        s->src.cm7 = s->armv7m[IMXRT1180_CPU_M7].cpu;
    }

    /* TRDC1..3 — return a sane HW config so eDMA/SAI drivers don't assert. */
    static const hwaddr trdc_base[IMXRT1180_NUM_TRDC] = {
        0x44270000, /* TRDC1 */
        0x42460000, /* TRDC2 */
        0x42810000, /* TRDC3 */
    };
    for (int i = 0; i < IMXRT1180_NUM_TRDC; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->trdc[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->trdc[i]), 0, trdc_base[i]);
    }
}

static const Property imxrt1180_soc_properties[] = {
    DEFINE_PROP_STRING("part", IMXRT1180State, part),
};

static void imxrt1180_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = imxrt1180_soc_realize;
    device_class_set_props(dc, imxrt1180_soc_properties);
    /* SoC is instantiated by the board; not user-creatable. */
    dc->user_creatable = false;
}

static const TypeInfo imxrt1180_soc_types[] = {
    {
        .name          = TYPE_IMXRT1180_SOC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180State),
        .instance_init = imxrt1180_soc_instance_init,
        .class_init    = imxrt1180_soc_class_init,
    },
};

DEFINE_TYPES(imxrt1180_soc_types)
