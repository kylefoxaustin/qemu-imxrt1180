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
#include "hw/misc/imxrt1180_flexspi.h"
#include "hw/misc/imxrt1180_ccm.h"
#include "hw/misc/imxrt1180_src.h"
#include "hw/misc/imxrt1180_trdc.h"
#include "hw/misc/imxrt1180_mu.h"
#include "hw/i2c/imxrt1180_lpi2c.h"
#include "hw/ssi/imxrt1180_lpspi.h"
#include "hw/timer/imxrt1180_lpit.h"
#include "hw/misc/imxrt1180_flexcan.h"
#include "hw/dma/imxrt1180_edma.h"
#include "hw/misc/imxrt1180_sai.h"
#include "hw/misc/imxrt1180_usbphy.h"
#include "hw/usb/imxrt1180_usb.h"
#include "hw/misc/imxrt1180_pwm.h"
#include "hw/misc/imxrt1180_eqdc.h"
#include "hw/gpio/imxrt1180_rgpio.h"
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
#define IMXRT1180_FLEXSPI1_BASE   0x28000000  /* FlexSPI1 NOR XIP window (NS)   */
#define IMXRT1180_FLEXSPI1_SIZE   0x01000000  /* 16 MiB (EVK flash)             */
#define IMXRT1180_FLEXSPI1_S_BASE 0x38000000  /* secure alias (TZ-M)            */
#define IMXRT1180_EXTRAM_BASE     0x14000000  /* ext RAM (Zephyr .data/.bss)    */
#define IMXRT1180_EXTRAM_SIZE     0x00800000  /* 8 MiB                          */

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

/* Inter-core MU1: MUA = CM33 side, MUB = CM7 side (NS bases); IRQ 21 each core. */
#define IMXRT1180_MU1_MUA_BASE    0x44220000
#define IMXRT1180_MU1_MUB_BASE    0x44230000
#define IMXRT1180_MU1_IRQ         21

/* LPI2C1..4 — controller-mode I2C (NS bases).  LPI2C2 is the EVK sensor bus. */
#define IMXRT1180_NUM_LPI2C       4

/* LPSPI1..4 — controller-mode SPI (NS bases). */
#define IMXRT1180_NUM_LPSPI       4

/* LPIT1..3 — low-power periodic interrupt timers (NS bases). */
#define IMXRT1180_NUM_LPIT        3

/* FlexCAN1..3 — CAN-FD-capable CAN controllers (NS bases). */
#define IMXRT1180_NUM_FLEXCAN     3

/* eDMA3 (32ch) + eDMA4 (64ch) — enhanced DMA engines (NS bases). */
#define IMXRT1180_NUM_EDMA        2

/* SAI1..4 — synchronous audio interface / I2S (NS bases). */
#define IMXRT1180_NUM_SAI         4
#define IMXRT1180_NUM_USBPHY      2       /* USBPHY1 @0x42CA0000, USBPHY2 @0x42CB0000 */
#define IMXRT1180_USBPHY1_BASE    0x42CA0000
#define IMXRT1180_USBPHY2_BASE    0x42CB0000
#define IMXRT1180_NUM_USB         2       /* USB_OTG1 @0x42C80000, USB_OTG2 @0x42C90000 */
#define IMXRT1180_USB_OTG1_BASE   0x42C80000
#define IMXRT1180_USB_OTG2_BASE   0x42C90000
#define IMXRT1180_USB_OTG1_IRQ    215
#define IMXRT1180_USB_OTG2_IRQ    214
#define IMXRT1180_NUM_PWM         4       /* eFlexPWM1..4 @0x4265/6/7/8_0000 */
/* Per module: 4 submodule IRQs (SM0..3) then the fault IRQ. */
#define IMXRT1180_PWM1_SM0_IRQ    24      /* PWM1: SM0..3 = 24..27, fault = 23 */
#define IMXRT1180_PWM1_FAULT_IRQ  23
#define IMXRT1180_PWM2_SM0_IRQ    171     /* PWM2: SM0..3 = 171..174, fault = 170 */
#define IMXRT1180_PWM2_FAULT_IRQ  170
#define IMXRT1180_PWM3_SM0_IRQ    176     /* PWM3: SM0..3 = 176..179, fault = 175 */
#define IMXRT1180_PWM3_FAULT_IRQ  175
#define IMXRT1180_PWM4_SM0_IRQ    181     /* PWM4: SM0..3 = 181..184, fault = 180 */
#define IMXRT1180_PWM4_FAULT_IRQ  180
#define IMXRT1180_NUM_EQDC        4       /* EQDC1..4 @0x4271/2/3/4_0000 */
#define IMXRT1180_EQDC1_BASE      0x42710000
#define IMXRT1180_EQDC1_IRQ       185     /* EQDC1..4 = 185..188 */

/* FlexSPI1 controller registers (NS, WAKEUPMIX) — distinct from the XIP window. */
#define IMXRT1180_FLEXSPI1_CTRL_BASE 0x425E0000

/* CCM — Clock Controller Module (AONMIX, NS). */
#define IMXRT1180_CCM_BASE           0x44450000

/* FlexSPI2 controller registers (NS, AONMIX). */
#define IMXRT1180_FLEXSPI2_CTRL_BASE 0x445E0000

/* RGPIO1..6 controllers (NS). */
#define IMXRT1180_NUM_RGPIO 6

/* TRDC1..3 — Trusted Resource Domain Controller. */
#define IMXRT1180_NUM_TRDC 3

/* SRC_GENERAL + BLK_CTRL_S_AONMIX — Cortex-M7 boot/release control. */
#define IMXRT1180_SRC_GENERAL_BASE   0x44460000
#define IMXRT1180_BLK_CTRL_S_AON_BASE 0x444F0000

/*
 * Cortex-M7 TCM as seen in the SYSTEM (M33) address view: ITCM @ 0x303C0000,
 * DTCM @ 0x30400000 (each 256 KiB).  The M33 loads/clears the M7 image here
 * (SDK InitCM7DMA clears 0x303C0000..0x30440000).  The M7's local view (ITCM
 * @ 0x0) is a follow-on; a system-view M7 image (INITVTOR in this window) boots
 * directly.
 */
#define IMXRT1180_CM7_TCM_BASE       0x303C0000
#define IMXRT1180_CM7_TCM_SIZE       0x00080000

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
    IMXRT1180FlexSPIState flexspi1_ctrl;       /* FlexSPI1 controller regs     */
    IMXRT1180CCMState    ccm;                  /* Clock Controller Module      */
    IMXRT1180FlexSPIState flexspi2_ctrl;       /* FlexSPI2 controller regs     */
    IMXRT1180RGPIOState  rgpio[IMXRT1180_NUM_RGPIO];   /* RGPIO1..6            */
    IMXRT1180SRCState    src;                  /* SRC + BLK_CTRL: M7 release   */
    IMXRT1180TRDCState   trdc[IMXRT1180_NUM_TRDC];     /* TRDC1..3             */
    IMXRT1180MUState     mu1;                          /* inter-core MU (M33<->M7) */
    IMXRT1180LPI2CState  lpi2c[IMXRT1180_NUM_LPI2C];   /* LPI2C1..4            */
    IMXRT1180LPSPIState  lpspi[IMXRT1180_NUM_LPSPI];   /* LPSPI1..4            */
    IMXRT1180LPITState   lpit[IMXRT1180_NUM_LPIT];     /* LPIT1..3             */
    IMXRT1180FlexCanState flexcan[IMXRT1180_NUM_FLEXCAN]; /* FlexCAN1..3       */
    IMXRT1180EDMAState   edma[IMXRT1180_NUM_EDMA];     /* eDMA3, eDMA4         */
    IMXRT1180SAIState    sai[IMXRT1180_NUM_SAI];       /* SAI1..4             */
    IMXRT1180USBPHYState usbphy[IMXRT1180_NUM_USBPHY];  /* USBPHY1..2 PLL       */
    IMXRT1180USBState    usb[IMXRT1180_NUM_USB];        /* USB_OTG1..2 (device) */
    IMXRT1180PWMState    pwm[IMXRT1180_NUM_PWM];         /* eFlexPWM1..4         */
    IMXRT1180EQDCState   eqdc[IMXRT1180_NUM_EQDC];       /* EQDC1..4 encoder     */
    MemoryRegion         cm7_tcm;              /* M7 TCM (system view @0x303C…)*/

    /* On-chip memories (CM33 view).  RAM-backed during bring-up. */
    MemoryRegion code_tcm;   /* ITCM  @ 0x0FFE0000 */
    MemoryRegion sys_tcm;    /* DTCM  @ 0x20000000 */
    MemoryRegion ocram1;     /* OCRAM1 @ 0x20484000 */
    MemoryRegion ocram2;     /* OCRAM2 @ 0x20500000 */
    MemoryRegion flexspi1;   /* FlexSPI1 NOR XIP @ 0x28000000 */
    MemoryRegion flexspi1_s_alias; /* secure alias @ 0x38000000 */
    MemoryRegion ext_ram;    /* external RAM @ 0x14000000 (Zephyr) */
    MemoryRegion periph_secure; /* TZ-M secure peripheral aperture @ 0x50000000 */

    Clock       *sysclk;
    Clock       *refclk;

    const IMXRT1180Config *cfg;  /* resolved from "part" at realize time */
    char                  *part; /* settable property: selects the config */
};

#endif /* HW_ARM_IMXRT1180_SOC_H */
