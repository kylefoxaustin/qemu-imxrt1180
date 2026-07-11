/*
 * Virtual-motor plant / closed-loop test (Cortex-M33).
 *
 * Drives the eFlexPWM with a small fixed stator voltage vector (electrical angle
 * 90 degrees, ~1.5 V) and lets the calibrated dq PMSM plant respond: the rotor
 * rotates to align its magnet with the field, the EQDC position follows it, and
 * the LPADC senses a real phase current.  Exercises the whole motor-control
 * frontier end to end -- PWM duty -> dq plant physics -> EQDC + ADC.
 *
 *   * with the PWM idle the rotor stands still (EQDC == 0, current == mid-scale)
 *   * with the vector applied the rotor aligns at electrical 90 deg; for the
 *     Pp = 4 M1 motor that is 90/4 = 22.5 deg mechanical = CPR*22.5/360 = 256
 *     counts, and a bounded phase current flows.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* PWM1 @ 0x4265_0000: submodule s registers at s*0x60. */
#define PWM1_BASE 0x42650000u
#define SM_INIT(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x02))
#define SM_VAL1(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x0E))
#define SM_VAL2(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x12))
#define SM_VAL3(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x16))
#define PWM_MCTRL  (*(volatile uint16_t *)(PWM1_BASE + 0x188))
#define MCTRL_LDOK 0x000Fu
#define MCTRL_RUN012 0x0700u          /* RUN submodules 0,1,2 */

/* EQDC1 @ 0x4271_0000: LPOS = live lower position counter. */
#define EQDC1_LPOS (*(volatile uint16_t *)(0x42710000u + 0x0E))

/* ADC1 @ 0x4260_0000. */
#define ADC1_BASE 0x42600000u
#define ADC_CTRL   (*(volatile uint32_t *)(ADC1_BASE + 0x10))
#define ADC_SWTRIG (*(volatile uint32_t *)(ADC1_BASE + 0x34))
#define ADC_TCTRL0 (*(volatile uint32_t *)(ADC1_BASE + 0xA0))
#define ADC_FCTRL0 (*(volatile uint32_t *)(ADC1_BASE + 0xE0))
#define ADC_CMDL0  (*(volatile uint32_t *)(ADC1_BASE + 0x100))
#define ADC_CMDH0  (*(volatile uint32_t *)(ADC1_BASE + 0x104))
#define ADC_RESFIFO0 (*(volatile uint32_t *)(ADC1_BASE + 0x300))
#define CTRL_ADCEN 0x1u
#define CTRL_CAL_REQ 0x8u
#define ADC_MID 0x8000

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

void reset_handler(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0] = (void (*)(void))STACK_TOP,
    [1] = reset_handler,
};

/* Software-trigger one conversion of ADC1 channel `ch` and pop its result. */
static uint32_t read_ch(unsigned ch)
{
    ADC_CMDL0 = ch;
    ADC_CMDH0 = 0;
    ADC_SWTRIG = 1u;
    while ((ADC_FCTRL0 & 0x1Fu) == 0u) {
    }
    return ADC_RESFIFO0 & 0xFFFF;
}

/* Read phase-B (channel 6) current code via a software-triggered conversion. */
static uint32_t read_phaseB(void)
{
    return read_ch(6);
}

/*
 * DC-bus voltage sense (channel 4 = mc_pmsm's M1_ADC1_UDCB), full scale
 * M1_U_DCB_MAX = 60.8 V.  This must be a REAL measurement of the bus the plant
 * drives (24 V), not the ADC's un-driven mid-scale placeholder.  0x8000 would
 * decode to 30.4 V -- a plausible-looking bus voltage that nothing measured,
 * and one an FOC loop would happily normalise its duty cycles against and run
 * its over/under-voltage protection off.  Guard against that regressing.
 */
#define UDCB_CH        4
#define UDCB_FS_MV     60800            /* 60.8 V full scale, in mV */
#define UDCB_EXPECT_MV 24000            /* the plant's bus voltage  */

static uint32_t udcb_mv(void)
{
    return (read_ch(UDCB_CH) * UDCB_FS_MV) / 0xFFFFu;
}

void reset_handler(void)
{
    int ok = 1;

    /* ADC1: enable, calibrate, one command sampling channel 6 (phase B). */
    ADC_CTRL  = CTRL_ADCEN | CTRL_CAL_REQ;
    ADC_CMDL0 = 6;
    ADC_CMDH0 = 0;
    ADC_TCTRL0 = (1u << 24);       /* TCMD = command 1, software trigger */

    /* Baseline: rotor idle -> position 0, current mid-scale. */
    if (EQDC1_LPOS != 0) { ok = 0; }
    uint32_t base_i = read_phaseB();
    if (base_i != ADC_MID) { ok = 0; }

    /*
     * Apply a small stator voltage vector at electrical angle 90 deg (~1.5 V on
     * a 24 V bus), period 1000: phase duties 0.50 / 0.554 / 0.446
     * (VAL3 = duty*500 = 250 / 277 / 223).
     */
    static const uint16_t val3[3] = { 250, 277, 223 };
    for (int s = 0; s < 3; s++) {
        SM_INIT(s) = (uint16_t)(-500);
        SM_VAL1(s) = 499;
        SM_VAL2(s) = (uint16_t)(-(int)val3[s]);
        SM_VAL3(s) = val3[s];
    }
    PWM_MCTRL = MCTRL_LDOK;
    PWM_MCTRL = MCTRL_RUN012;

    /* Let the rotor swing to alignment (~256 counts) and settle. */
    uint16_t pos = 0;
    uint32_t guard = 0;
    while (pos < 200) {
        pos = EQDC1_LPOS;
        if (++guard > 300000000u) { ok = 0; break; }
    }
    for (volatile int i = 0; i < 2000000; i++) {   /* let it settle */
    }
    pos = EQDC1_LPOS;

    /* A bounded phase current must flow (channel 6 deviates from mid-scale). */
    uint32_t run_i = read_phaseB();
    int32_t di = (int32_t)run_i - ADC_MID;
    if (di < 0) { di = -di; }
    if (di < 1000 || di > 0x7000) { ok = 0; }      /* present but not railed */

    /*
     * DC-bus sense must report the plant's real bus (24 V +/- 1 V) -- and must
     * NOT be the un-driven mid-scale placeholder (which decodes to 30.4 V).
     */
    int udcb_ok = 1;
    uint32_t vbus = udcb_mv();
    if (read_ch(UDCB_CH) == ADC_MID) { udcb_ok = 0; }   /* still a placeholder! */
    if (vbus < UDCB_EXPECT_MV - 1000u || vbus > UDCB_EXPECT_MV + 1000u) {
        udcb_ok = 0;
    }
    if (!udcb_ok) { ok = 0; }

    if (ok && pos >= 150 && pos <= 400) {
        puts_("MOTOR: PASS - dq PMSM aligned to the field + bounded phase current sensed\r\n");
        puts_("MOTOR: PASS - DC-bus sense reads the plant's real 24V bus (not a placeholder)\r\n");
    } else if (!udcb_ok) {
        puts_("MOTOR: FAIL - DC-bus channel is not driven by the plant\r\n");
    } else {
        puts_("MOTOR: FAIL - plant did not close the loop\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
