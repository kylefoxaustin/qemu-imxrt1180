/*
 * NXP i.MX RT1180 crossover MCU SoC (Arm Cortex-M33 + Cortex-M7)
 *
 * Part-agnostic SoC container for the i.MX RT1180 family.  The headline part
 * is the fully-loaded MIMXRT1189 (dual-core: a secure Cortex-M33 boot core and
 * a Cortex-M7 main core).  Adding a family variant is a single table entry in
 * imxrt1180_soc.c (see IMXRT1180Config); the rest of the model is generic.
 *
 * Source of truth: the MIMXRT1189 CMSIS headers (bases/IRQs/masks) and the
 * i.MX RT1180 Reference Manual (register semantics).  Never invent an offset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_IMXRT1180_SOC_H
#define HW_ARM_IMXRT1180_SOC_H

#include "hw/core/sysbus.h"
#include "hw/arm/armv7m.h"
#include "hw/char/imxrt1180_lpuart.h"
#include "hw/misc/imxrt1180_anadig.h"
#include "hw/misc/imxrt1180_rtwdog.h"
#include "hw/misc/imxrt1180_s3mu.h"
#include "hw/core/clock.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_SOC "imxrt1180-soc"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180State, IMXRT1180_SOC)

/*
 * Heterogeneous dual-core: the i.MX RT1180 pairs a Cortex-M33 (the secure boot
 * core, "Secure CPU Platform") with a Cortex-M7 (the "Main CPU Platform").  The
 * M33 boots from ROM/FlexSPI, brings up security (TRDC), and releases the M7.
 * cpu0 == M33 (boot), cpu1 == M7 (released by the M33).
 */
#define IMXRT1180_MAX_CPUS 2
#define IMXRT1180_CPU_M33  0   /* primary / boot / secure */
#define IMXRT1180_CPU_M7   1   /* main application core   */

/* --- CM33-view on-chip memory map (MIMXRT1189, from the SDK linker scripts) -- *
 * These are the addresses the boot Cortex-M33 sees.  The Cortex-M7 sees a
 * different local TCM layout (ITCM @ 0x0, larger TCMs) plus SDRAM/HyperRAM;
 * that view is added when cpu1 is wired.  All values VERIFIED against
 * MIMXRT1189xxxxx_cm33_flexspi_nor.ld.
 */
#define IMXRT1180_CODE_TCM_BASE   0x0FFE0000  /* CM33 ITCM (code TCM)   128 KiB */
#define IMXRT1180_CODE_TCM_SIZE   0x00020000
#define IMXRT1180_SYS_TCM_BASE    0x20000000  /* CM33 DTCM (system TCM) 128 KiB */
#define IMXRT1180_SYS_TCM_SIZE    0x00020000
#define IMXRT1180_OCRAM1_BASE     0x20484000  /* OCRAM1 (first 16K TRDC-blocked)*/
#define IMXRT1180_OCRAM1_SIZE     0x0007C000
#define IMXRT1180_OCRAM2_BASE     0x20500000  /* OCRAM2                 256 KiB */
#define IMXRT1180_OCRAM2_SIZE     0x00040000
#define IMXRT1180_FLEXSPI1_BASE   0x28000000  /* FlexSPI1 NOR XIP window   8 MiB*/
#define IMXRT1180_FLEXSPI1_SIZE   0x00800000

/*
 * Peripheral window.  The RT1180 is TrustZone-M with two power-domain aliases:
 * AONMIX   non-secure 0x4400_0000 / secure 0x5400_0000
 * WAKEUPMIX non-secure 0x4200_0000 / secure 0x5200_0000
 * One catch-all spanning 0x4000_0000..0x5FFF_FFFF covers all four.  Run with
 * -d unimp,guest_errors to see each access, then replace slices with models.
 */
#define IMXRT1180_PERIPH_BASE     0x40000000
#define IMXRT1180_PERIPH_SIZE     0x20000000

/*
 * LPUART1 = the MIMXRT1180-EVK debug console (BOARD_DEBUG_UART = LPUART1).
 * Non-secure base 0x4438_0000 (secure alias 0x5438_0000); NVIC line 19.
 * Verified against the MIMXRT1189 CMSIS header.
 */
#define IMXRT1180_LPUART1_BASE    0x44380000
#define IMXRT1180_LPUART1_IRQ     19

/* ANADIG analog clock block (OSC + PLL + PMU), AONMIX non-secure. */
#define IMXRT1180_ANADIG_BASE     0x44480000

/*
 * RTWDOG1..5 — 2 in the AON mix, 3 in the WAKEUP mix (non-secure bases).
 * SystemInit unlocks + disables each early in boot.
 */
#define IMXRT1180_NUM_RTWDOG      5

/* Messaging Unit (RT domain) to the EdgeLock secure enclave (ELE/S3), NS base. */
#define IMXRT1180_MU_RT_S3MU_BASE 0x47540000

/* Per-core architectural configuration (M33 and M7 differ). */
typedef struct IMXRT1180CoreConfig {
    const char *cpu_type;      /* ARM_CPU_TYPE_NAME("cortex-m33" | "cortex-m7") */
    uint8_t     num_prio_bits; /* __NVIC_PRIO_BITS (M33=3, M7=4)                */
} IMXRT1180CoreConfig;

/* Per-SKU configuration.  Confirm every field against the part RM/CMSIS. */
typedef struct IMXRT1180Config {
    const char *name;          /* e.g. "MIMXRT1189"                            */
    uint32_t    num_cpus;      /* MVP wires the M33 only; M7 is follow-on       */
    uint32_t    num_irq;       /* NVIC external IRQ lines (1189: 239, 0..238)   */
    IMXRT1180CoreConfig core[IMXRT1180_MAX_CPUS];
} IMXRT1180Config;

struct IMXRT1180State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    ARMv7MState  armv7m[IMXRT1180_MAX_CPUS];   /* cpu0 = M33, cpu1 = M7        */
    MemoryRegion cpu_mem[IMXRT1180_MAX_CPUS];  /* per-core alias of the SoC map*/

    IMXRT1180LPUARTState lpuart1;              /* debug console (LPUART1)      */
    IMXRT1180AnadigState anadig;               /* analog clock (OSC/PLL)       */
    IMXRT1180RTWDOGState rtwdog[IMXRT1180_NUM_RTWDOG]; /* RTWDOG1..5            */
    IMXRT1180S3MUState   mu_rt_s3;             /* MU to EdgeLock enclave (ELE) */

    /* On-chip memories (CM33 view).  RAM-backed during bring-up. */
    MemoryRegion code_tcm;   /* ITCM  @ 0x0FFE0000 */
    MemoryRegion sys_tcm;    /* DTCM  @ 0x20000000 */
    MemoryRegion ocram1;     /* OCRAM1 @ 0x20484000 */
    MemoryRegion ocram2;     /* OCRAM2 @ 0x20500000 */
    MemoryRegion flexspi1;   /* FlexSPI1 NOR XIP @ 0x28000000 */

    Clock       *sysclk;
    Clock       *refclk;

    const IMXRT1180Config *cfg;  /* resolved from "part" at realize time */
    char                  *part; /* settable property: selects the config */
};

#endif /* HW_ARM_IMXRT1180_SOC_H */
