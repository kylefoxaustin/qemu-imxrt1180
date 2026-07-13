/*
 * NXP i.MX RT1180 CCM — Clock Controller Module.
 *
 * THIS BLOCK USED TO BE A REGISTER FILE WEARING A CLOCK TREE'S NAME.  It stored
 * the MUX/DIV the guest wrote into CLOCK_ROOT[n].CONTROL and NOTHING DOWNSTREAM
 * EVER READ THEM.  Two consequences, and they are the same bug seen from both ends:
 *
 *   1. OBSERVE[].FREQUENCY_CURRENT returned a FABRICATED 6 MHz.  The guest's
 *      CLOCK_GetFreqFromObs() multiplies it by (CCM_OBS_DIV+1) and divides a baud
 *      rate by the result.  THE NUMBER LANDED IN THE GUEST'S ARITHMETIC.  The old
 *      source said so, honestly: "a consumer that needs an exact frequency is
 *      getting an approximation -- flagged here rather than presented as measured
 *      silicon."  THE FLAG WAS A C COMMENT.  FIRMWARE CANNOT READ C COMMENTS.
 *      CLAUDE.md has forbidden exactly this for months ("an out-of-band flag is not
 *      an honest fault") and the code did it anyway.
 *
 *   2. Every timer in the machine ticked at a hardcoded constant.  The SoC wired
 *      sysclk/refclk to the CPU and drove NOT ONE peripheral clock; six blocks each
 *      opened with `if (!s->clk) s->clk = DEFAULT;`, a fallback that made the
 *      missing wiring invisible.  Measured against what the SDK's own
 *      CLOCK_GetRootClockFreq() computes: GPT 10x slow, LPIT/TPM 5.5x slow, LPTMR
 *      3.3x slow, QTMR 1.8x fast, eFlexPWM 1.5x fast.  NOT ONE OF THE SIX MATCHED.
 *      (mcxn947qemu's OSTIMER, six times over: "a ?: is not a safety net, it is a
 *      place for a bug to live where no test will ever look.")
 *
 * So: COMPUTE IT.  CLAUDE.md rule 4 -- decline only what you genuinely cannot
 * produce, and a clock tree whose inputs are all in registers we already hold is
 * not something we cannot produce.
 *
 *   root_hz(n) = source_hz(MUX_TABLE[n][CONTROL.MUX]) / (CONTROL.DIV + 1)
 *
 * MUX_TABLE is s_clockSourceName[74][4] from fsl_clock.c -- a device fact, taken
 * mechanically from the SDK, not retyped.  The PLL/OSC frequencies are computed
 * from the ANADIG registers, the same arithmetic as CLOCK_GetPllFreq().
 *
 * Offsets/counts verified against the MIMXRT1189 CMSIS PERI_CCM.h.
 * Reset values are the RM's cold-POR column (the reset-value gate checks them).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_ccm.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* ---- Register geometry.  ALL of this is from PERI_CCM.h, none of it invented. */
#define CCM_ROOT_BASE          0x0000      /* CLOCK_ROOT[74], step 0x80          */
#define CCM_ROOT_STEP          0x80
#define CCM_ROOT_CONTROL       0x00
#define CCM_ROOT_STATUS0       0x20
#define CCM_ROOT_AUTHEN        0x30

#define CCM_OBS_BASE           0x4400      /* OBSERVE[2], step 0x80              */
#define CCM_OBS_STEP           0x80
#define CCM_OBS_COUNT          2
#define CCM_OBS_CONTROL        0x4400
#define CCM_OBS_AUTHEN         0x4430
#define CCM_OBS_FREQ_CURRENT   0x4440
#define CCM_OBS_FREQ_MIN       0x4444
#define CCM_OBS_FREQ_MAX       0x4448
#define CCM_OBS_PERIOD_MIN     0x4454
#define CCM_OBS_HIGH_MIN       0x4464
#define CCM_OBS_LOW_MIN        0x4474

#define CCM_GPR_PRIV_BASE      0x4C00      /* GPR_PRIVATE[4], step 0x20          */
#define CCM_GPR_PRIV_STEP      0x20
#define CCM_GPR_PRIV_COUNT     4
#define CCM_GPR_PRIV_AUTHEN    0x10        /* +SET/CLR/TOG at +0x14/0x18/0x1C    */

#define CCM_OSCPLL_BASE        0x5000      /* OSCPLL[25], step 0x40              */
#define CCM_OSCPLL_STEP        0x40
#define CCM_OSCPLL_COUNT       25
#define CCM_OSCPLL_DIRECT      0x00
#define CCM_OSCPLL_STATUS0     0x20
#define CCM_OSCPLL_STATUS1     0x24
#define CCM_OSCPLL_AUTHEN      0x30

#define CCM_LPCG_BASE          0x8000      /* LPCG[149], step 0x40               */
#define CCM_LPCG_STEP          0x40
#define CCM_LPCG_COUNT         149
#define CCM_LPCG_DIRECT        0x00
#define CCM_LPCG_STATUS0       0x20
#define CCM_LPCG_STATUS1       0x24
#define CCM_LPCG_AUTHEN        0x30

#define LPCG_ON                0x1u

/* CLOCK_ROOT[n].CONTROL fields (CCM_CLOCK_ROOT_CONTROL_{DIV,MUX}_MASK). */
#define ROOT_DIV_MASK          0xFFu
#define ROOT_MUX_SHIFT         8
#define ROOT_MUX_MASK          0x3u
#define ROOT_OFF               (1u << 24)  /* CONTROL.OFF: root gated off        */

/* OBSERVE[n].CONTROL: SELECT picks the observed clock; OFF gates the observer. */
#define OBS_SELECT_MASK        0x1FFu
#define OBS_OFF                (1u << 24)
#define OBS_DIV_MASK           0xFFu
#define OBS_DIV_SHIFT          16

/* ---- The clock sources a root MUX can select (fsl_clock.h clock_name_t). */
typedef enum {
    CLK_OSC_RC_24M, CLK_OSC_RC_400M, CLK_OSC_24M,
    CLK_ARM_PLL, CLK_AUDIO_PLL,
    CLK_SYS_PLL1, CLK_SYS_PLL1_DIV2, CLK_SYS_PLL1_DIV5,
    CLK_SYS_PLL2, CLK_SYS_PLL2_PFD0, CLK_SYS_PLL2_PFD1,
    CLK_SYS_PLL2_PFD2, CLK_SYS_PLL2_PFD3,
    CLK_SYS_PLL3, CLK_SYS_PLL3_DIV2, CLK_SYS_PLL3_PFD0,
    CLK_SYS_PLL3_PFD1, CLK_SYS_PLL3_PFD2, CLK_SYS_PLL3_PFD3,
} IMXRT1180ClkSrc;

/*
 * s_clockSourceName[74][4], fsl_clock.c.  Taken MECHANICALLY from the SDK: a
 * per-root mux table is a device fact and retyping 296 entries by hand is how you
 * fabricate one.  The row count is asserted below against CMSIS's
 * CCM_CLOCK_ROOT_COUNT -- a different file, parsed by a different tool.
 * (Generating it, the first pass silently produced 73 rows: the last entry has no
 * trailing comma and the regex skipped it.  A count from an independent source is
 * the only reason that did not become root 73 reading off the end of the table.)
 */
static const IMXRT1180ClkSrc ccm_root_mux[IMXRT1180_CCM_NROOT][4] = {
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_ARM_PLL      , CLK_SYS_PLL3      },  /*  0 M7 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3     , CLK_ARM_PLL       },  /*  1 M33 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1     , CLK_SYS_PLL2_PFD1 },  /*  2 EDGELOCK */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL2     , CLK_SYS_PLL3_PFD2 },  /*  3 BUS_AON */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL2     , CLK_SYS_PLL3_PFD1 },  /*  4 BUS_WAKEUP */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3     , CLK_SYS_PLL2_PFD1 },  /*  5 WAKEUP_AXI */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL1_DIV5 },  /*  6 SWO_TRACE */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_OSC_24M      , CLK_SYS_PLL3_DIV2 },  /*  7 M33_SYSTICK */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_OSC_24M      , CLK_SYS_PLL3_DIV2 },  /*  8 M7_SYSTICK */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL1_DIV5 },  /*  9 FLEXIO1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL1_DIV5 },  /* 10 FLEXIO2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 11 LPIT3 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 12 LPTIMER1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 13 LPTIMER2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 14 LPTIMER3 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 15 TPM2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 16 TPM4 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 17 TPM5 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 18 TPM6 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 19 GPT1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 20 GPT2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_PFD0, CLK_SYS_PLL2_PFD2 },  /* 21 FLEXSPI1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_PFD0, CLK_SYS_PLL2_PFD2 },  /* 22 FLEXSPI2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_PFD0, CLK_SYS_PLL2_PFD2 },  /* 23 FLEXSPI_SLV */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 24 CAN1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 25 CAN2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 26 CAN3 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 27 LPUART0102 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 28 LPUART0304 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 29 LPUART0506 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 30 LPUART0708 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 31 LPUART0910 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 32 LPUART1112 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 33 LPI2C0102 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 34 LPI2C0304 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 35 LPI2C0506 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 36 LPSPI0102 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 37 LPSPI0304 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 38 LPSPI0506 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 39 I3C1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 40 I3C2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 41 USDHC1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 42 USDHC2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 43 SEMC */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 44 ADC1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 45 ADC2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 46 ACMP */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 47 ECAT */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 48 ENET */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 49 TMR_1588 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 50 NETC */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 51 MAC0 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 52 MAC1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 53 MAC2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 54 MAC3 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 55 MAC4 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV2, CLK_SYS_PLL2_PFD1 },  /* 56 SERDES0 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV2, CLK_SYS_PLL2_PFD1 },  /* 57 SERDES1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV2, CLK_SYS_PLL2_PFD1 },  /* 58 SERDES2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 59 SERDES0_1G */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 60 SERDES1_1G */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_SYS_PLL2_PFD1 },  /* 61 SERDES2_1G */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 62 XCELBUSX */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 63 XRIOCU4 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL2_PFD3 },  /* 64 MCTRL */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_AUDIO_PLL    , CLK_SYS_PLL3_PFD2 },  /* 65 SAI1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_AUDIO_PLL    , CLK_SYS_PLL3_PFD2 },  /* 66 SAI2 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_AUDIO_PLL    , CLK_SYS_PLL3_PFD2 },  /* 67 SAI3 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_AUDIO_PLL    , CLK_SYS_PLL3_PFD2 },  /* 68 SAI4 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_AUDIO_PLL    , CLK_SYS_PLL3_PFD2 },  /* 69 SPDIF */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_AUDIO_PLL    , CLK_SYS_PLL3_PFD2 },  /* 70 ASRC */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_AUDIO_PLL     },  /* 71 MIC */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL3_DIV2, CLK_SYS_PLL1_DIV2 },  /* 72 CKO1 */
    { CLK_OSC_RC_24M   , CLK_OSC_RC_400M  , CLK_SYS_PLL1_DIV5, CLK_ARM_PLL       },  /* 73 CKO2 */
};

/* fsl_clock.h */
#define XTAL_FREQ          24000000u
#define PLL_SYS1_1G_FREQ   1000000000u
#define PLL_SYS2_528_FREQ  (XTAL_FREQ * 22u)   /* PLL_SYS2_528_MFI = 22 */
#define PLL_SYS3_480_FREQ  (XTAL_FREQ * 20u)   /* PLL_SYS3_480_MFI = 20 */

/* ANADIG_PLL, relative to the ANADIG window base. */
#define ANADIG_ARM_PLL_CTRL      0x4000
#define ANADIG_SYS_PLL3_PFD      0x4030
#define ANADIG_SYS_PLL2_PFD      0x4070
#define ARM_PLL_DIV_SELECT_MASK  0xFFu
#define ARM_PLL_POST_DIV_SHIFT   13
#define ARM_PLL_POST_DIV_MASK    0x7u
#define PFD_FRAC_MASK            0x3Fu

static uint32_t anadig_reg(IMXRT1180CCMState *s, hwaddr off)
{
    return s->anadig ? s->anadig->regs[off / 4] : 0;
}

/* CLOCK_GetPfdFreq(): pllFreq * 18 / frac.  frac == 0 means the PFD is off. */
static uint32_t ccm_pfd_hz(IMXRT1180CCMState *s, hwaddr pfd_reg,
                           uint32_t pll_hz, unsigned pfd)
{
    uint32_t frac = (anadig_reg(s, pfd_reg) >> (8 * pfd)) & PFD_FRAC_MASK;

    return frac ? (uint32_t)(((uint64_t)pll_hz * 18u) / frac) : 0;
}

/* CLOCK_GetPllFreq(kCLOCK_PllArm): XTAL / (2 * 2^(POST_DIV+1)) * DIV_SELECT. */
static uint32_t ccm_arm_pll_hz(IMXRT1180CCMState *s)
{
    uint32_t ctrl = anadig_reg(s, ANADIG_ARM_PLL_CTRL);
    uint32_t div_select = ctrl & ARM_PLL_DIV_SELECT_MASK;
    uint32_t post = (ctrl >> ARM_PLL_POST_DIV_SHIFT) & ARM_PLL_POST_DIV_MASK;

    return (uint32_t)(((uint64_t)XTAL_FREQ / (2ull << (post + 1))) * div_select);
}

static uint32_t ccm_src_hz(IMXRT1180CCMState *s, IMXRT1180ClkSrc src)
{
    switch (src) {
    case CLK_OSC_RC_24M:    return 24000000u;
    case CLK_OSC_RC_400M:   return 400000000u;
    case CLK_OSC_24M:       return XTAL_FREQ;
    case CLK_ARM_PLL:       return ccm_arm_pll_hz(s);
    case CLK_SYS_PLL1:      return PLL_SYS1_1G_FREQ;
    case CLK_SYS_PLL1_DIV2: return PLL_SYS1_1G_FREQ / 2;
    case CLK_SYS_PLL1_DIV5: return PLL_SYS1_1G_FREQ / 5;
    case CLK_SYS_PLL2:      return PLL_SYS2_528_FREQ;
    case CLK_SYS_PLL3:      return PLL_SYS3_480_FREQ;
    case CLK_SYS_PLL3_DIV2: return PLL_SYS3_480_FREQ / 2;

    case CLK_SYS_PLL2_PFD0: case CLK_SYS_PLL2_PFD1:
    case CLK_SYS_PLL2_PFD2: case CLK_SYS_PLL2_PFD3:
        return ccm_pfd_hz(s, ANADIG_SYS_PLL2_PFD, PLL_SYS2_528_FREQ,
                          src - CLK_SYS_PLL2_PFD0);

    case CLK_SYS_PLL3_PFD0: case CLK_SYS_PLL3_PFD1:
    case CLK_SYS_PLL3_PFD2: case CLK_SYS_PLL3_PFD3:
        return ccm_pfd_hz(s, ANADIG_SYS_PLL3_PFD, PLL_SYS3_480_FREQ,
                          src - CLK_SYS_PLL3_PFD0);

    case CLK_AUDIO_PLL:
        /* The audio PLL's fractional divider is not modelled.  Return 0 -- a
         * REFUSAL, not a number.  Callers must not divide by it, and no timer in
         * this machine selects it.  Answering "some plausible MHz" here is how the
         * fabricated 6 MHz got in.  ("Decline what you cannot produce"; the audio
         * path is honestly-open work, see PERIPHERALS.md.) */
        qemu_log_mask(LOG_UNIMP, "%s: AUDIO_PLL frequency not modelled; "
                      "reporting 0 (no frequency), not a guess\n", __func__);
        return 0;
    }
    return 0;
}

uint32_t imxrt1180_ccm_root_hz(IMXRT1180CCMState *s, unsigned root)
{
    uint32_t ctrl, div, hz;
    unsigned mux;

    if (root >= IMXRT1180_CCM_NROOT) {
        return 0;
    }
    ctrl = s->regs[(CCM_ROOT_BASE + root * CCM_ROOT_STEP + CCM_ROOT_CONTROL) / 4];
    if (ctrl & ROOT_OFF) {
        return 0;                       /* the guest gated this root off */
    }
    mux = (ctrl >> ROOT_MUX_SHIFT) & ROOT_MUX_MASK;
    div = (ctrl & ROOT_DIV_MASK) + 1;   /* the field holds DIV-1 */

    hz = ccm_src_hz(s, ccm_root_mux[root][mux]);
    return hz / div;
}

/* ---- OBSERVE.  The block counts edges of the clock its SELECT field picks. */
static bool ccm_obs_slice(hwaddr off, unsigned reg, unsigned *slice)
{
    for (unsigned n = 0; n < CCM_OBS_COUNT; n++) {
        if (off == reg + n * CCM_OBS_STEP) {
            *slice = n;
            return true;
        }
    }
    return false;
}

/*
 * OBSERVE[n].SELECT indexes the observable-clock list.  Selectors 0..73 are the
 * clock ROOTS -- which is what CLOCK_GetFreqFromObs() uses and what we can
 * actually compute.  The remaining selectors observe raw OSC/PLL nodes and are
 * NOT modelled: they report 0.
 *
 * 0 IS THE RM's RESET VALUE AND IT IS AN HONEST "NOTHING MEASURED".  It is also
 * what the block reads before the guest starts it.  The old model returned a
 * hardcoded 6 MHz here unconditionally -- including at reset, where the manual
 * says 0 -- and the reset-value gate caught it as 0x005b8d80.
 */
static uint32_t ccm_obs_hz(IMXRT1180CCMState *s, unsigned slice)
{
    uint32_t ctrl = s->regs[(CCM_OBS_CONTROL + slice * CCM_OBS_STEP) / 4];
    uint32_t sel = ctrl & OBS_SELECT_MASK;
    uint32_t div = ((ctrl >> OBS_DIV_SHIFT) & OBS_DIV_MASK) + 1;

    if (ctrl & OBS_OFF) {
        return 0;                       /* observer not running: nothing measured */
    }
    if (sel >= IMXRT1180_CCM_NROOT) {
        qemu_log_mask(LOG_UNIMP, "%s: OBSERVE%u selects source %u (not a clock "
                      "root); this model can only measure roots, reporting 0\n",
                      __func__, slice, sel);
        return 0;
    }
    /* The observer divides what it counts; CLOCK_GetFreqFromObs multiplies back. */
    return imxrt1180_ccm_root_hz(s, sel) / div;
}

static uint64_t imxrt1180_ccm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180CCMState *s = IMXRT1180_CCM(opaque);
    unsigned slice;

    if (offset + 4 > IMXRT1180_CCM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    /* LPCG STATUS0.ON mirrors DIRECT.ON, so CLOCK_ControlGate's poll completes. */
    if (offset >= CCM_LPCG_BASE + CCM_LPCG_STATUS0 &&
        offset <  CCM_LPCG_BASE + CCM_LPCG_COUNT * CCM_LPCG_STEP &&
        ((offset - CCM_LPCG_BASE - CCM_LPCG_STATUS0) % CCM_LPCG_STEP) == 0) {
        unsigned gate = (offset - CCM_LPCG_BASE - CCM_LPCG_STATUS0) / CCM_LPCG_STEP;
        uint32_t direct = s->regs[(CCM_LPCG_BASE + gate * CCM_LPCG_STEP) / 4];
        return (direct & LPCG_ON) ? LPCG_ON : 0;
    }

    if (ccm_obs_slice(offset, CCM_OBS_FREQ_CURRENT, &slice)) {
        return ccm_obs_hz(s, slice);            /* COMPUTED, not fabricated */
    }

    return s->regs[offset / 4];
}

static void imxrt1180_ccm_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMXRT1180CCMState *s = IMXRT1180_CCM(opaque);

    if (offset + 4 > IMXRT1180_CCM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imxrt1180_ccm_ops = {
    .read = imxrt1180_ccm_read,
    .write = imxrt1180_ccm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * RESET.  A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM -- IT IS A CLAIM, AND
 * THE GUEST BELIEVES IT.  This block was a bare memset(regs, 0), which is 804
 * false statements about the silicon:
 *
 *   every LPCG gate and every OSCPLL says OFF at reset;   silicon says ON  (0x1)
 *   every AUTHEN says 0;                                  silicon says 0xffff0000
 *
 * Values are the RM's cold-POR reset column; offsets are from PERI_CCM.h.  The
 * reset-value gate checks all 804 independently, against a golden built by a
 * different tool from a different document.
 */
static void imxrt1180_ccm_reset(DeviceState *dev)
{
    IMXRT1180CCMState *s = IMXRT1180_CCM(dev);
    unsigned n;

    memset(s->regs, 0, sizeof(s->regs));

#define CCM_SET(off, val)  (s->regs[(off) / 4] = (val))

    for (n = 0; n < IMXRT1180_CCM_NROOT; n++) {
        hwaddr b = CCM_ROOT_BASE + n * CCM_ROOT_STEP;
        CCM_SET(b + CCM_ROOT_AUTHEN, 0xFFFF0000);
    }
    for (n = 0; n < CCM_OSCPLL_COUNT; n++) {
        hwaddr b = CCM_OSCPLL_BASE + n * CCM_OSCPLL_STEP;
        CCM_SET(b + CCM_OSCPLL_DIRECT,  0x00000001);   /* enabled out of reset */
        CCM_SET(b + CCM_OSCPLL_STATUS0, 0x00000001);
        CCM_SET(b + CCM_OSCPLL_STATUS1, 0x0000FFFF);
        CCM_SET(b + CCM_OSCPLL_AUTHEN,  0xFFFF0000);
    }
    for (n = 0; n < CCM_LPCG_COUNT; n++) {
        hwaddr b = CCM_LPCG_BASE + n * CCM_LPCG_STEP;
        CCM_SET(b + CCM_LPCG_DIRECT,  0x00000001);     /* gate ON out of reset */
        CCM_SET(b + CCM_LPCG_STATUS0, 0x00000001);
        CCM_SET(b + CCM_LPCG_STATUS1, 0x0000FFFF);
        CCM_SET(b + CCM_LPCG_AUTHEN,  0xFFFF0000);
    }
    for (n = 0; n < CCM_GPR_PRIV_COUNT; n++) {
        hwaddr b = CCM_GPR_PRIV_BASE + n * CCM_GPR_PRIV_STEP + CCM_GPR_PRIV_AUTHEN;
        CCM_SET(b + 0x0, 0xFFFF0000);                  /* AUTHEN            */
        CCM_SET(b + 0x4, 0xFFFF0000);                  /* AUTHEN_SET        */
        CCM_SET(b + 0x8, 0xFFFF0000);                  /* AUTHEN_CLR        */
        CCM_SET(b + 0xC, 0xFFFF0000);                  /* AUTHEN_TOG        */
    }
    for (n = 0; n < CCM_OBS_COUNT; n++) {
        hwaddr b = n * CCM_OBS_STEP;
        CCM_SET(CCM_OBS_AUTHEN       + b + 0x0, 0xFFFF0000);
        CCM_SET(CCM_OBS_AUTHEN       + b + 0x4, 0xFFFF0000);
        CCM_SET(CCM_OBS_AUTHEN       + b + 0x8, 0xFFFF0000);
        CCM_SET(CCM_OBS_AUTHEN       + b + 0xC, 0xFFFF0000);
        /* The min-trackers start at their maxima so the first sample wins. */
        CCM_SET(CCM_OBS_FREQ_MIN     + b, 0xFFFFFFC0);
        CCM_SET(CCM_OBS_PERIOD_MIN   + b, 0xFFFFFFFF);
        CCM_SET(CCM_OBS_HIGH_MIN     + b, 0xFFFFFFFF);
        CCM_SET(CCM_OBS_LOW_MIN      + b, 0xFFFFFFFF);
    }
#undef CCM_SET
}

static void imxrt1180_ccm_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180CCMState *s = IMXRT1180_CCM(dev);

    /*
     * An UNDER-FILLED mux table zero-fills, and enum 0 is CLK_OSC_RC_24M -- so a
     * row the generator missed would read as a perfectly plausible 24 MHz, which
     * is EXACTLY how six hardcoded timer defaults hid in this tree for months.
     * The generator did drop a row on its first pass (the last entry in
     * fsl_clock.c has no trailing comma).  So prove the far end is populated:
     * CKO2's mux2 is SYS_PLL1_DIV5, which no zero-fill can produce.
     */
    QEMU_BUILD_BUG_ON(ARRAY_SIZE(ccm_root_mux) != IMXRT1180_CCM_NROOT);
    assert(ccm_root_mux[IMXRT1180_CCM_NROOT - 1][2] == CLK_SYS_PLL1_DIV5);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_ccm_ops, s,
                          TYPE_IMXRT1180_CCM, IMXRT1180_CCM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imxrt1180_ccm = {
    .name = TYPE_IMXRT1180_CCM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180CCMState, IMXRT1180_CCM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imxrt1180_ccm_props[] = {
    DEFINE_PROP_LINK("anadig", IMXRT1180CCMState, anadig,
                     TYPE_IMXRT1180_ANADIG, IMXRT1180AnadigState *),
};

static void imxrt1180_ccm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_ccm_realize;
    device_class_set_legacy_reset(dc, imxrt1180_ccm_reset);
    dc->vmsd = &vmstate_imxrt1180_ccm;
    device_class_set_props(dc, imxrt1180_ccm_props);
}

static const TypeInfo imxrt1180_ccm_types[] = {
    {
        .name          = TYPE_IMXRT1180_CCM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180CCMState),
        .class_init    = imxrt1180_ccm_class_init,
    },
};

DEFINE_TYPES(imxrt1180_ccm_types)
