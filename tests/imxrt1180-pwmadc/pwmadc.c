/*
 * eFlexPWM -> XBAR -> LPADC synchronised-sampling test (Cortex-M33).
 *
 * Proves the motor-control sync chain end to end, with no CPU involvement in the
 * triggering: the PWM submodule's periodic output trigger is routed through the
 * XBAR to the ADC's hardware trigger input, so every PWM period launches an ADC
 * conversion whose result lands in the FIFO tagged with its trigger source.
 *
 *   PWM1 SM0 OutTrig0 (XBAR in 74) --> XBAR --> ADC12_HW_TRIG0 (XBAR out 140)
 *                                              --> ADC1 hardware trigger 0
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* XBAR1 @ 0x4275_0000 — SEL register for output 140 = SEL[70] @ 70*2 = 0x8C. */
#define XBAR1_SEL70 (*(volatile uint16_t *)(0x42750000u + 0x8C))
#define XBAR_IN_PWM1_SM0_TRIG0 74u

/* ADC1 @ 0x4260_0000. */
#define ADC1_BASE 0x42600000u
#define ADC_CTRL   (*(volatile uint32_t *)(ADC1_BASE + 0x10))
#define ADC_STAT   (*(volatile uint32_t *)(ADC1_BASE + 0x14))
#define ADC_TCTRL0 (*(volatile uint32_t *)(ADC1_BASE + 0xA0))
#define ADC_FCTRL0 (*(volatile uint32_t *)(ADC1_BASE + 0xE0))
#define ADC_CMDL0  (*(volatile uint32_t *)(ADC1_BASE + 0x100))
#define ADC_CMDH0  (*(volatile uint32_t *)(ADC1_BASE + 0x104))
#define ADC_RESFIFO0 (*(volatile uint32_t *)(ADC1_BASE + 0x300))
#define CTRL_ADCEN   0x1u
#define CTRL_CAL_REQ 0x8u
#define TCTRL_HTEN   0x1u
#define RESFIFO_VALID 0x80000000u

/* PWM1 @ 0x4265_0000, submodule 0. */
#define PWM1_BASE 0x42650000u
#define SM0_INIT  (*(volatile uint16_t *)(PWM1_BASE + 0x02))
#define SM0_VAL1  (*(volatile uint16_t *)(PWM1_BASE + 0x0E))
#define SM0_VAL2  (*(volatile uint16_t *)(PWM1_BASE + 0x12))
#define SM0_VAL3  (*(volatile uint16_t *)(PWM1_BASE + 0x16))
#define SM0_TCTRL (*(volatile uint16_t *)(PWM1_BASE + 0x2A))
#define PWM_MCTRL (*(volatile uint16_t *)(PWM1_BASE + 0x188))
#define PWM_OUT_TRIG_EN_VAL4 0x10u    /* TCTRL.OUT_TRIG_EN bit for VAL4 */
#define MCTRL_LDOK 0x000Fu

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

void reset_handler(void)
{
    int ok = 1;

    /* Route PWM1 SM0 OutTrig0 (input 74) to ADC12_HW_TRIG0 (output 140). */
    XBAR1_SEL70 = XBAR_IN_PWM1_SM0_TRIG0;

    /* ADC1: enable + calibrate, one command, hardware trigger 0 -> command 1. */
    ADC_CTRL  = CTRL_ADCEN | CTRL_CAL_REQ;
    ADC_CMDL0 = 5;                       /* channel 5 */
    ADC_CMDH0 = 0;                       /* single command */
    ADC_TCTRL0 = (1u << 24) | TCTRL_HTEN; /* TCMD=1, hardware trigger enabled */
    ADC_FCTRL0 = 0;

    /* No conversions should have happened before the PWM runs. */
    if ((ADC_FCTRL0 & 0x1Fu) != 0u) { ok = 0; }

    /* PWM1 SM0: center-aligned period 1000, emit an output trigger each period. */
    SM0_INIT = (uint16_t)(-500);
    SM0_VAL1 = 499;
    SM0_VAL2 = (uint16_t)(-250);
    SM0_VAL3 = 250;
    SM0_TCTRL = PWM_OUT_TRIG_EN_VAL4;
    PWM_MCTRL = MCTRL_LDOK;
    PWM_MCTRL = (1u << 8);               /* RUN submodule 0 */

    /* Wait for the PWM to trigger some ADC conversions via the XBAR. */
    uint32_t guard = 0;
    while ((ADC_FCTRL0 & 0x1Fu) == 0u) {
        if (++guard > 20000000u) { ok = 0; break; }
    }

    /* A trigger-sourced result must be present and tagged source 0. */
    uint32_t r = ADC_RESFIFO0;
    if (!(r & RESFIFO_VALID)) { ok = 0; }
    if (((r >> 16) & 0x7u) != 0u) { ok = 0; }   /* TSRC == 0 */

    if (ok) {
        puts_("PWM->XBAR->ADC: PASS - PWM period triggers a synchronised ADC conversion\r\n");
    } else {
        puts_("PWM->XBAR->ADC: FAIL - sync chain did not trigger a conversion\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
