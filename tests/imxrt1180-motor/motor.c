/*
 * Virtual-motor plant / closed-loop test (Cortex-M33).
 *
 * Drives the eFlexPWM with a fixed stator voltage vector (electrical angle 90
 * degrees) and lets the virtual PMSM plant respond: the rotor rotates to align
 * with the field, the EQDC position counter follows it, and the LPADC senses a
 * real phase current.  This exercises the whole motor-control frontier end to
 * end -- PWM duty -> plant physics -> EQDC position + ADC current.
 *
 *   * with the PWM idle the rotor stands still (EQDC == 0, current == mid-scale)
 *   * with the vector applied the rotor turns toward ~1/4 revolution (CPR/4 =
 *     1024 counts) and a phase current flows.
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

/* Read phase-B (channel 6) current code via a software-triggered conversion. */
static uint32_t read_phaseB(void)
{
    ADC_SWTRIG = 1u;
    while ((ADC_FCTRL0 & 0x1Fu) == 0u) {
    }
    return ADC_RESFIFO0 & 0xFFFF;
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
     * Apply a stator voltage vector at electrical angle 90 deg, period 1000:
     *   phase A duty 0.50, phase B 0.76, phase C 0.24  (VAL3 = duty*500).
     */
    static const uint16_t val3[3] = { 250, 380, 120 };
    for (int s = 0; s < 3; s++) {
        SM_INIT(s) = (uint16_t)(-500);
        SM_VAL1(s) = 499;
        SM_VAL2(s) = (uint16_t)(-(int)val3[s]);
        SM_VAL3(s) = val3[s];
    }
    PWM_MCTRL = MCTRL_LDOK;
    PWM_MCTRL = MCTRL_RUN012;

    /* Let the rotor swing toward the field (~1024 counts). */
    uint16_t pos = 0;
    uint32_t guard = 0;
    while (pos < 600) {
        pos = EQDC1_LPOS;
        if (++guard > 300000000u) { ok = 0; break; }
    }

    /* A phase current must now flow (channel 6 deviates from mid-scale). */
    uint32_t run_i = read_phaseB();
    int32_t di = (int32_t)run_i - ADC_MID;
    if (di < 0) { di = -di; }
    if (di < 1000) { ok = 0; }

    if (ok && pos >= 600 && pos <= 1400) {
        puts_("MOTOR: PASS - rotor spun to the commanded field + phase current sensed\r\n");
    } else {
        puts_("MOTOR: FAIL - plant did not close the loop\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
