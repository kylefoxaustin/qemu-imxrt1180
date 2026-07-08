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
#include "hw/core/irq.h"             /* qemu_allocate_irqs */
#include "hw/i2c/i2c.h"             /* i2c_slave_create_simple */
#include "hw/sensor/fxls8974.h"     /* TYPE_FXLS8974 */
#include "hw/misc/imxrt1180_periphrdy.h"
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
    object_initialize_child(obj, "lpuart2", &s->lpuart2, TYPE_IMXRT1180_LPUART);
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
    object_initialize_child(obj, "mu1", &s->mu1, TYPE_IMXRT1180_MU);
    for (int i = 0; i < IMXRT1180_NUM_LPI2C; i++) {
        g_autofree char *iname = g_strdup_printf("lpi2c%d", i + 1);
        object_initialize_child(obj, iname, &s->lpi2c[i], TYPE_IMXRT1180_LPI2C);
    }
    for (int i = 0; i < IMXRT1180_NUM_LPSPI; i++) {
        g_autofree char *sname = g_strdup_printf("lpspi%d", i + 1);
        object_initialize_child(obj, sname, &s->lpspi[i], TYPE_IMXRT1180_LPSPI);
    }
    for (int i = 0; i < IMXRT1180_NUM_LPIT; i++) {
        g_autofree char *tname = g_strdup_printf("lpit%d", i + 1);
        object_initialize_child(obj, tname, &s->lpit[i], TYPE_IMXRT1180_LPIT);
    }
    for (int i = 0; i < IMXRT1180_NUM_FLEXCAN; i++) {
        g_autofree char *cname = g_strdup_printf("flexcan%d", i + 1);
        object_initialize_child(obj, cname, &s->flexcan[i],
                                TYPE_IMXRT1180_FLEXCAN);
    }
    for (int i = 0; i < IMXRT1180_NUM_EDMA; i++) {
        g_autofree char *dname = g_strdup_printf("edma%d", i + 3);  /* eDMA3/4 */
        object_initialize_child(obj, dname, &s->edma[i], TYPE_IMXRT1180_EDMA);
    }
    for (int i = 0; i < IMXRT1180_NUM_SAI; i++) {
        g_autofree char *aname = g_strdup_printf("sai%d", i + 1);
        object_initialize_child(obj, aname, &s->sai[i], TYPE_IMXRT1180_SAI);
    }
    for (int i = 0; i < IMXRT1180_NUM_USBPHY; i++) {
        g_autofree char *pname = g_strdup_printf("usbphy%d", i + 1);
        object_initialize_child(obj, pname, &s->usbphy[i], TYPE_IMXRT1180_USBPHY);
    }
    for (int i = 0; i < IMXRT1180_NUM_USB; i++) {
        g_autofree char *uname = g_strdup_printf("usb%d", i + 1);
        object_initialize_child(obj, uname, &s->usb[i], TYPE_IMXRT1180_USB);
    }
    for (int i = 0; i < IMXRT1180_NUM_PWM; i++) {
        g_autofree char *wname = g_strdup_printf("pwm%d", i + 1);
        object_initialize_child(obj, wname, &s->pwm[i], TYPE_IMXRT1180_PWM);
    }
    for (int i = 0; i < IMXRT1180_NUM_EQDC; i++) {
        g_autofree char *qname = g_strdup_printf("eqdc%d", i + 1);
        object_initialize_child(obj, qname, &s->eqdc[i], TYPE_IMXRT1180_EQDC);
    }
    for (int i = 0; i < IMXRT1180_NUM_ADC; i++) {
        g_autofree char *aname = g_strdup_printf("adc%d", i + 1);
        object_initialize_child(obj, aname, &s->adc[i], TYPE_IMXRT1180_ADC);
    }
    object_initialize_child(obj, "xbar1", &s->xbar1, TYPE_IMXRT1180_XBAR);
    object_initialize_child(obj, "motor", &s->motor, TYPE_IMXRT1180_MOTOR);
    for (int i = 0; i < IMXRT1180_NUM_TMR; i++) {
        g_autofree char *tn = g_strdup_printf("tmr%d", i + 1);
        object_initialize_child(obj, tn, &s->tmr[i], TYPE_IMXRT1180_TMR);
    }
    for (int i = 0; i < IMXRT1180_NUM_LPTMR; i++) {
        g_autofree char *n = g_strdup_printf("lptmr%d", i + 1);
        object_initialize_child(obj, n, &s->lptmr[i], TYPE_IMXRT1180_LPTMR);
    }
    for (int i = 0; i < IMXRT1180_NUM_GPT; i++) {
        g_autofree char *n = g_strdup_printf("gpt%d", i + 1);
        object_initialize_child(obj, n, &s->gpt[i], TYPE_IMXRT1180_GPT);
    }
    for (int i = 0; i < IMXRT1180_NUM_TPM; i++) {
        g_autofree char *n = g_strdup_printf("tpm%d", i + 1);
        object_initialize_child(obj, n, &s->tpm[i], TYPE_IMXRT1180_TPM);
    }
    for (int i = 0; i < IMXRT1180_NUM_SEMA42; i++) {
        g_autofree char *n = g_strdup_printf("sema%d", i + 1);
        object_initialize_child(obj, n, &s->sema42[i], TYPE_IMXRT1180_SEMA42);
    }
    for (int i = 0; i < IMXRT1180_NUM_CMP; i++) {
        g_autofree char *n = g_strdup_printf("cmp%d", i + 1);
        object_initialize_child(obj, n, &s->cmp[i], TYPE_IMXRT1180_CMP);
    }
    object_initialize_child(obj, "vref", &s->vref, TYPE_IMXRT1180_VREF);
    object_initialize_child(obj, "netc", &s->netc, TYPE_IMXRT1180_NETC);
    for (int i = 0; i < 6; i++) {
        g_autofree char *mn = g_strdup_printf("msgintr%d", i + 1);
        object_initialize_child(obj, mn, &s->msgintr[i], TYPE_IMXRT1180_MSGINTR);
    }
    for (int i = 0; i < IMXRT1180_NUM_USDHC; i++) {
        g_autofree char *n = g_strdup_printf("usdhc%d", i + 1);
        object_initialize_child(obj, n, &s->usdhc[i], TYPE_IMX_USDHC);
    }
    for (int i = 0; i < IMXRT1180_NUM_TRDC; i++) {
        g_autofree char *tname = g_strdup_printf("trdc%d", i + 1);
        object_initialize_child(obj, tname, &s->trdc[i], TYPE_IMXRT1180_TRDC);
    }

    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
    s->refclk = qdev_init_clock_in(DEVICE(s), "refclk", NULL, NULL, 0);
}

/*
 * Fan an XBAR ADC12_HW_TRIG line out to both LPADCs' matching trigger input.
 * `line` is the HW_TRIG index (0..7); the ADCs' per-trigger HTEN gate decides
 * which one actually launches a conversion.
 */
static void imxrt1180_adc_trig_fanout(void *opaque, int line, int level)
{
    IMXRT1180State *s = opaque;

    for (int i = 0; i < IMXRT1180_NUM_ADC; i++) {
        qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&s->adc[i]), "adc-trig", line),
                     level);
    }
}

/* Create a register-backed readiness block (see imxrt1180_periphrdy). */
static void imxrt1180_add_rdy(IMXRT1180State *s, const char *name, hwaddr base,
                              uint32_t size)
{
    DeviceState *d = qdev_new(TYPE_IMXRT1180_PERIPHRDY);
    qdev_prop_set_uint32(d, "mmsize", size);
    object_property_add_child(OBJECT(s), name, OBJECT(d));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, base);
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
    /* TZ-M secure alias of the FlexSPI NOR XIP window (secure M33 / Zephyr). */
    memory_region_init_alias(&s->flexspi1_s_alias, OBJECT(dev),
                             "imxrt1180.flexspi1-nor.s", &s->flexspi1, 0,
                             IMXRT1180_FLEXSPI1_SIZE);
    memory_region_add_subregion(system_memory, IMXRT1180_FLEXSPI1_S_BASE,
                                &s->flexspi1_s_alias);
    /* External RAM window (0x14000000) — Zephyr .data/.bss. */
    memory_region_init_ram(&s->ext_ram, OBJECT(dev), "imxrt1180.ext-ram",
                           IMXRT1180_EXTRAM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, IMXRT1180_EXTRAM_BASE,
                                &s->ext_ram);

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

    /*
     * TrustZone-M: the secure peripheral aperture (0x5000_0000..0x5FFF_FFFF)
     * mirrors the non-secure one (0x4000_0000..0x4FFF_FFFF) — every peripheral
     * has a secure alias 0x1000_0000 above its NS base.  Secure firmware (e.g.
     * Zephyr on the secure M33) reaches peripherals through those aliases, so
     * mirror the whole NS window there with a single alias (overrides the
     * catch-all by priority; the modelled devices below it show through).
     */
    memory_region_init_alias(&s->periph_secure, OBJECT(dev),
                             "imxrt1180.periph.s", system_memory,
                             IMXRT1180_PERIPH_BASE, 0x10000000);
    memory_region_add_subregion(system_memory, 0x50000000, &s->periph_secure);

    /*
     * NETC (integrated PCIe Ethernet controller + ENETC + switch + PTP) lives at
     * 0x6000_0000, outside the peripheral window.  The model covers the ENETC
     * endpoint path (IERB/PCI/capability registers, EMDIO+PHY, TX->RX BD
     * loopback) exercised by the netc_txrx_transfer example.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->netc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->netc), 0, IMXRT1180_NETC_BASE);

    /* LPUART1 — debug console.  Binds host serial_hd(0); IRQ 19 -> M33 NVIC. */
    qdev_prop_set_chr(DEVICE(&s->lpuart1), "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpuart1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpuart1), 0, IMXRT1180_LPUART1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpuart1), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M33]),
                                        IMXRT1180_LPUART1_IRQ));

    /*
     * LPUART2 — board-to-board link port.  Binds host serial_hd(1) when the
     * user supplies a second -serial (e.g. a socket chardev to another board);
     * otherwise it is a stand-alone modelled UART.  IRQ 20 -> M33 NVIC.
     */
    qdev_prop_set_chr(DEVICE(&s->lpuart2), "chardev", serial_hd(1));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpuart2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpuart2), 0, IMXRT1180_LPUART2_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpuart2), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M33]),
                                        IMXRT1180_LPUART2_IRQ));

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

    /*
     * TRDC1..3 — return a sane HW config so eDMA/SAI drivers don't assert.
     * ncm-mask marks each instance's non-CPU (DMA/peripheral) masters so the
     * SoC TRDC-setup's Set{,Non}ProcessorDomainAssignment DACFG.NCM asserts pass
     * (derived from the SoC master map + how the RT118x SoC init classifies each
     * master): TRDC1 DMA3(2); TRDC2 DAP(2)/CoreSight(3)/DMA4(4); TRDC3
     * USDHC1(0)/USDHC2(1)/Usb(3)/FlexspiFlr(4).
     */
    static const hwaddr trdc_base[IMXRT1180_NUM_TRDC] = {
        0x44270000, /* TRDC1 */
        0x42460000, /* TRDC2 */
        0x42810000, /* TRDC3 */
    };
    static const uint32_t trdc_ncm_mask[IMXRT1180_NUM_TRDC] = {
        0x04,  /* TRDC1: DMA3 */
        0x1C,  /* TRDC2: DAP, CoreSight, DMA4 */
        0x1B,  /* TRDC3: USDHC1, USDHC2, Usb, FlexspiFlr */
    };
    for (int i = 0; i < IMXRT1180_NUM_TRDC; i++) {
        qdev_prop_set_uint32(DEVICE(&s->trdc[i]), "ncm-mask", trdc_ncm_mask[i]);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->trdc[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->trdc[i]), 0, trdc_base[i]);
    }

    /*
     * Inter-core MU1: MMIO 0 = MUA (CM33 @ 0x4422_0000), MMIO 1 = MUB (CM7 @
     * 0x4423_0000); IRQ 0 -> CM33 NVIC, IRQ 1 -> CM7 NVIC (both MU1_IRQn=21).
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mu1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mu1), 0, IMXRT1180_MU1_MUA_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mu1), 1, IMXRT1180_MU1_MUB_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->mu1), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M33]),
                                        IMXRT1180_MU1_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->mu1), 1,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M7]),
                                        IMXRT1180_MU1_IRQ));

    /* LPI2C1..4 (controller mode); each exposes an I2C bus for device models. */
    static const struct { hwaddr base; unsigned irq; } lpi2c_cfg[] = {
        { 0x44340000, 13 },  /* LPI2C1 */
        { 0x44350000, 14 },  /* LPI2C2 (EVK sensor bus) */
        { 0x42530000, 62 },  /* LPI2C3 */
        { 0x42540000, 63 },  /* LPI2C4 */
    };
    for (int i = 0; i < IMXRT1180_NUM_LPI2C; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpi2c[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpi2c[i]), 0, lpi2c_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpi2c[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M33]),
                                            lpi2c_cfg[i].irq));
    }

    /* EVK on-board FXLS8974CF accelerometer (U115) on LPI2C2 at 0x19. */
    i2c_slave_create_simple(s->lpi2c[1].bus, TYPE_FXLS8974, 0x19);

    /* LPSPI1..4 (controller mode); each exposes an SSI bus for device models. */
    static const struct { hwaddr base; unsigned irq; } lpspi_cfg[] = {
        { 0x44360000, 16 },  /* LPSPI1 */
        { 0x44370000, 17 },  /* LPSPI2 */
        { 0x42550000, 65 },  /* LPSPI3 */
        { 0x42560000, 66 },  /* LPSPI4 */
    };
    for (int i = 0; i < IMXRT1180_NUM_LPSPI; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpspi[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpspi[i]), 0, lpspi_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpspi[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M33]),
                                            lpspi_cfg[i].irq));
    }

    /* LPIT1..3 (periodic interrupt timers). */
    static const struct { hwaddr base; unsigned irq; } lpit_cfg[] = {
        { 0x442F0000, 15  },  /* LPIT1 */
        { 0x424C0000, 64  },  /* LPIT2 */
        { 0x42CC0000, 149 },  /* LPIT3 */
    };
    for (int i = 0; i < IMXRT1180_NUM_LPIT; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpit[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpit[i]), 0, lpit_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpit[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M33]),
                                            lpit_cfg[i].irq));
    }

    /*
     * FlexCAN1..3.  Loopback works stand-alone; for board-to-board, link each
     * instance's "canbus" property to a -object can-bus (bridged out with a
     * can-host-chardev) — the model TXes/RXes real CAN frames when linked.
     */
    static const struct { hwaddr base; unsigned irq; } flexcan_cfg[] = {
        { 0x443A0000, 8   },  /* FlexCAN1 */
        { 0x425B0000, 51  },  /* FlexCAN2 */
        { 0x445B0000, 191 },  /* FlexCAN3 */
    };
    for (int i = 0; i < IMXRT1180_NUM_FLEXCAN; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->flexcan[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexcan[i]), 0, flexcan_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexcan[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M33]),
                                            flexcan_cfg[i].irq));
    }

    /*
     * eDMA3 (32 channels, per-channel IRQ 95..126) and eDMA4 (64 channels,
     * grouped IRQ: channels 2k/2k+1/2k+32/2k+33 share IRQ 128+k).  The model
     * exposes one qemu_irq per channel; grouped channels connect to the same
     * NVIC input (the NVIC ORs them).
     */
    DeviceState *m33 = DEVICE(&s->armv7m[IMXRT1180_CPU_M33]);
    static const struct { hwaddr base; unsigned nch; } edma_cfg[] = {
        { 0x44000000, 32 },  /* eDMA3 */
        { 0x42000000, 64 },  /* eDMA4 */
    };
    for (int i = 0; i < IMXRT1180_NUM_EDMA; i++) {
        qdev_prop_set_uint32(DEVICE(&s->edma[i]), "num-channels",
                             edma_cfg[i].nch);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->edma[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->edma[i]), 0, edma_cfg[i].base);
        for (unsigned ch = 0; ch < edma_cfg[i].nch; ch++) {
            unsigned irq = (i == 0) ? (95 + ch)              /* eDMA3 per-ch */
                                    : (128 + (ch % 32) / 2); /* eDMA4 grouped */
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->edma[i]), ch,
                               qdev_get_gpio_in(m33, irq));
        }
    }

    /* SAI1..4 (I2S) — control-register bring-up level. */
    static const struct { hwaddr base; unsigned irq; } sai_cfg[] = {
        { 0x443B0000, 45  },  /* SAI1 */
        { 0x42BB0000, 198 },  /* SAI2 */
        { 0x42BC0000, 199 },  /* SAI3 */
        { 0x42BD0000, 154 },  /* SAI4 */
    };
    for (int i = 0; i < IMXRT1180_NUM_SAI; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->sai[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->sai[i]), 0, sai_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->sai[i]), 0,
                           qdev_get_gpio_in(m33, sai_cfg[i].irq));
    }

    /* USBPHY1..2 — USB 2.0 HS PHY PLL readiness (lock reported once powered). */
    static const hwaddr usbphy_base[IMXRT1180_NUM_USBPHY] = {
        IMXRT1180_USBPHY1_BASE,
        IMXRT1180_USBPHY2_BASE,
    };
    for (int i = 0; i < IMXRT1180_NUM_USBPHY; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->usbphy[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->usbphy[i]), 0, usbphy_base[i]);
    }

    /* USB_OTG1..2 — ChipIdea USB-HS device-mode controller (no host attached). */
    static const struct { hwaddr base; unsigned irq; } usb_cfg[IMXRT1180_NUM_USB] = {
        { IMXRT1180_USB_OTG1_BASE, IMXRT1180_USB_OTG1_IRQ },
        { IMXRT1180_USB_OTG2_BASE, IMXRT1180_USB_OTG2_IRQ },
    };
    for (int i = 0; i < IMXRT1180_NUM_USB; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->usb[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->usb[i]), 0, usb_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->usb[i]), 0,
                           qdev_get_gpio_in(m33, usb_cfg[i].irq));
    }

    /* eFlexPWM1..4 — motor-control PWM.  Per module: SM0..3 IRQ then fault. */
    static const struct { hwaddr base; unsigned sm0_irq, fault_irq; }
        pwm_cfg[IMXRT1180_NUM_PWM] = {
        { 0x42650000, IMXRT1180_PWM1_SM0_IRQ, IMXRT1180_PWM1_FAULT_IRQ },
        { 0x42660000, IMXRT1180_PWM2_SM0_IRQ, IMXRT1180_PWM2_FAULT_IRQ },
        { 0x42670000, IMXRT1180_PWM3_SM0_IRQ, IMXRT1180_PWM3_FAULT_IRQ },
        { 0x42680000, IMXRT1180_PWM4_SM0_IRQ, IMXRT1180_PWM4_FAULT_IRQ },
    };
    for (int i = 0; i < IMXRT1180_NUM_PWM; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->pwm[i]);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, pwm_cfg[i].base);
        for (int sm = 0; sm < IMXRT1180_PWM_NSM; sm++) {   /* SM0..3 */
            sysbus_connect_irq(sbd, sm,
                qdev_get_gpio_in(m33, pwm_cfg[i].sm0_irq + sm));
        }
        sysbus_connect_irq(sbd, IMXRT1180_PWM_NSM,          /* fault */
            qdev_get_gpio_in(m33, pwm_cfg[i].fault_irq));
    }

    /* EQDC1..4 — quadrature encoder (register-accurate; no plant drives it). */
    for (int i = 0; i < IMXRT1180_NUM_EQDC; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->eqdc[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->eqdc[i]), 0,
                        IMXRT1180_EQDC1_BASE + (hwaddr)i * 0x10000);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->eqdc[i]), 0,
                           qdev_get_gpio_in(m33, IMXRT1180_EQDC1_IRQ + i));
    }

    /* LPADC1..2 — SAR ADC.  HW trigger inputs to be routed from PWM via XBAR. */
    static const struct { hwaddr base; unsigned irq; } adc_cfg[IMXRT1180_NUM_ADC] = {
        { IMXRT1180_ADC1_BASE, IMXRT1180_ADC1_IRQ },
        { IMXRT1180_ADC2_BASE, IMXRT1180_ADC2_IRQ },
    };
    for (int i = 0; i < IMXRT1180_NUM_ADC; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc[i]), 0, adc_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc[i]), 0,
                           qdev_get_gpio_in(m33, adc_cfg[i].irq));
    }

    /* XBAR1 — signal crossbar (carries the eFlexPWM edge to the ADC). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->xbar1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->xbar1), 0, IMXRT1180_XBAR1_BASE);

    /*
     * Motor-control PWM->ADC sync wiring:
     *   eFlexPWM1 submodule trigger outputs -> XBAR inputs (Flexpwm1 PwmN OutTrig0)
     *   XBAR ADC12_HW_TRIG outputs -> both LPADCs' hardware trigger inputs
     * (each HW_TRIG line reaches both ADCs; the per-trigger TCTRL.HTEN gate
     * decides which one actually converts).
     */
    for (int sm = 0; sm < IMXRT1180_PWM_NSM; sm++) {
        qdev_connect_gpio_out_named(DEVICE(&s->pwm[0]), "pwm-trig", sm,
            qdev_get_gpio_in_named(DEVICE(&s->xbar1), "xbar-in",
                                   IMXRT1180_XBAR1_IN_PWM1_TRIG0 + 2 * sm));
    }
    qemu_irq *adc_trig_fan = qemu_allocate_irqs(imxrt1180_adc_trig_fanout, s,
                                                IMXRT1180_ADC_NTRIG);
    for (int n = 0; n < IMXRT1180_ADC_NTRIG; n++) {
        qdev_connect_gpio_out_named(DEVICE(&s->xbar1), "xbar-out",
                                    IMXRT1180_XBAR1_OUT_ADC_HWTRIG0 + n,
                                    adc_trig_fan[n]);
    }

    /*
     * Virtual-motor plant: a first-order PMSM wired to eFlexPWM1 (phase duties),
     * EQDC1 (rotor position) and both LPADCs (phase-current samples).  It closes
     * the FOC loop -- a running control loop actually spins a virtual rotor.
     */
    s->motor.pwm   = &s->pwm[0];
    s->motor.eqdc  = &s->eqdc[0];
    s->motor.adc_a = &s->adc[0];
    s->motor.adc_c = &s->adc[1];
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->motor), errp)) {
        return;
    }

    /* QuadTimer TMR1..8 — 4-channel 16-bit timers. */
    static const unsigned tmr_irq[IMXRT1180_NUM_TMR] = { 0, 233, 164, 151, 4, 5, 6, 7 };
    for (int i = 0; i < IMXRT1180_NUM_TMR; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->tmr[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->tmr[i]), 0,
                        IMXRT1180_TMR1_BASE + (hwaddr)i * 0x10000);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->tmr[i]), 0,
                           qdev_get_gpio_in(m33, tmr_irq[i]));
    }

    /* LPTMR1..3, GPT1..2, TPM1..6 — low-power / general / PWM timers. */
    static const struct { hwaddr base; unsigned irq; } lptmr_cfg[IMXRT1180_NUM_LPTMR] = {
        { 0x44300000, 18 }, { 0x424D0000, 67 }, { 0x42CD0000, 150 } };
    for (int i = 0; i < IMXRT1180_NUM_LPTMR; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->lptmr[i]), errp)) { return; }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->lptmr[i]), 0, lptmr_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lptmr[i]), 0, qdev_get_gpio_in(m33, lptmr_cfg[i].irq));
    }
    static const struct { hwaddr base; unsigned irq; } gpt_cfg[IMXRT1180_NUM_GPT] = {
        { 0x446C0000, 209 }, { 0x42EC0000, 210 } };
    for (int i = 0; i < IMXRT1180_NUM_GPT; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpt[i]), errp)) { return; }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpt[i]), 0, gpt_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpt[i]), 0, qdev_get_gpio_in(m33, gpt_cfg[i].irq));
    }
    static const struct { hwaddr base; unsigned irq; } tpm_cfg[IMXRT1180_NUM_TPM] = {
        { 0x44310000, 36 }, { 0x44320000, 37 }, { 0x424E0000, 75 },
        { 0x424F0000, 76 }, { 0x42500000, 77 }, { 0x42510000, 78 } };
    for (int i = 0; i < IMXRT1180_NUM_TPM; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->tpm[i]), errp)) { return; }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->tpm[i]), 0, tpm_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->tpm[i]), 0, qdev_get_gpio_in(m33, tpm_cfg[i].irq));
    }

    /* SEMA1..2 (hardware semaphores), CMP1..4 (comparators), VREF. */
    static const hwaddr sema_base[IMXRT1180_NUM_SEMA42] = { 0x44260000, 0x42450000 };
    for (int i = 0; i < IMXRT1180_NUM_SEMA42; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->sema42[i]), errp)) { return; }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->sema42[i]), 0, sema_base[i]);
    }
    for (int i = 0; i < IMXRT1180_NUM_CMP; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->cmp[i]), errp)) { return; }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->cmp[i]), 0, 0x42DC0000 + (hwaddr)i * 0x10000);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->cmp[i]), 0, qdev_get_gpio_in(m33, 200 + i));
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->vref), errp)) { return; }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->vref), 0, 0x42E30000);

    /* USDHC1..2 — SD/MMC host controllers (upstream imx-usdhc model). */
    static const struct { hwaddr base; unsigned irq; } usdhc_cfg[IMXRT1180_NUM_USDHC] = {
        { 0x42850000, 86 }, { 0x42860000, 87 } };
    for (int i = 0; i < IMXRT1180_NUM_USDHC; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->usdhc[i]), errp)) { return; }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->usdhc[i]), 0, usdhc_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->usdhc[i]), 0,
                           qdev_get_gpio_in(m33, usdhc_cfg[i].irq));
    }

    /*
     * Register-accurate readiness blocks for peripherals whose data path is not
     * modelled (config sticks + reads back; no transfers) -- SINC (Sigma-Delta),
     * SPDIF, PDM (audio), SEMC (ext memory ctrl), I3C, USBNC (USB non-core).
     */
    imxrt1180_add_rdy(s, "sinc1", 0x42BF0000, 0x1000);
    imxrt1180_add_rdy(s, "sinc2", 0x42C00000, 0x1000);
    imxrt1180_add_rdy(s, "sinc3", 0x42C10000, 0x1000);
    imxrt1180_add_rdy(s, "spdif", 0x42BA0000, 0x1000);
    imxrt1180_add_rdy(s, "pdm",   0x42BE0000, 0x1000);
    imxrt1180_add_rdy(s, "semc",  0x42910000, 0x1000);
    imxrt1180_add_rdy(s, "i3c1",  0x44330000, 0x1000);
    imxrt1180_add_rdy(s, "i3c2",  0x42520000, 0x1000);
    imxrt1180_add_rdy(s, "usbnc1", 0x42C80200, 0x100);
    imxrt1180_add_rdy(s, "usbnc2", 0x42C90200, 0x100);
    for (int i = 0; i < 6; i++) {          /* MSGINTR1..6 message-interrupt routers */
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->msgintr[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->msgintr[i]), 0,
                        0x428A0000 + (hwaddr)i * 0x10000);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->msgintr[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[IMXRT1180_CPU_M33]),
                                            218 + i));   /* MSGINTR1..6 = 218..223 */
    }
    imxrt1180_add_rdy(s, "flexio1", 0x425C0000, 0x1000);  /* flexible I/O engine */
    imxrt1180_add_rdy(s, "flexio2", 0x425D0000, 0x1000);
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
