/*
 * Virtual-motor plant — MAGNETIC SATURATION value-test (Cortex-M33).
 *
 * Saturation has no static signature (steady-state id = v/Rs is independent of L)
 * -- it lives in the current TRANSIENT: the rise time constant tau = Ld/Rs is
 * current-INDEPENDENT for a linear inductor but SHRINKS with current when the
 * iron saturates. So we measure the SysTick time for the d-axis current to rise
 * to 50% of its steady value for a SMALL step and a LARGE step, and take the
 * RATIO t_large/t_small: 1.0 for a linear inductor, < 1.0 when saturating. The
 * ratio cancels the SysTick clock rate (only a monotonic counter is needed).
 *
 * The step is a clean d-axis transient with the rotor STATIONARY: at reset
 * theta=0, a voltage vector along the alpha axis is purely on the d-axis
 * (vd=V, vq=0), and a d-axis current makes no torque (it is aligned with the
 * magnet), so the rotor does not move and iq stays 0. Phase-A current == id here,
 * read on ADC ch5. Between the two measurements the PWM is stopped: the tristated
 * inverter free-wheels, zeroing id, so the large step also starts from 0.
 *
 * This firmware only REPORTS the two raw SysTick counts; run.sh computes the
 * closed-form golden ratio per i_sat and sweeps i_sat. Needs -icount (SysTick
 * deterministic) + a high plant rate (fine transient resolution).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u
#define PWM1 0x42650000u
#define SM_INIT(s) (*(volatile uint16_t *)(PWM1 + (s)*0x60 + 0x02))
#define SM_VAL1(s) (*(volatile uint16_t *)(PWM1 + (s)*0x60 + 0x0E))
#define SM_VAL2(s) (*(volatile uint16_t *)(PWM1 + (s)*0x60 + 0x12))
#define SM_VAL3(s) (*(volatile uint16_t *)(PWM1 + (s)*0x60 + 0x16))
#define MCTRL  (*(volatile uint16_t *)(PWM1 + 0x188))
#define LDOK   0x000Fu
#define RUN012 0x0700u
#define ADC1 0x42600000u
#define ADC_CTRL   (*(volatile uint32_t *)(ADC1 + 0x10))
#define ADC_SWTRIG (*(volatile uint32_t *)(ADC1 + 0x34))
#define ADC_TCTRL0 (*(volatile uint32_t *)(ADC1 + 0xA0))
#define ADC_FCTRL0 (*(volatile uint32_t *)(ADC1 + 0xE0))
#define ADC_CMDL0  (*(volatile uint32_t *)(ADC1 + 0x100))
#define ADC_CMDH0  (*(volatile uint32_t *)(ADC1 + 0x104))
#define ADC_RESFIFO0 (*(volatile uint32_t *)(ADC1 + 0x300))
#define ADC_MID 0x8000
#define SYST_CSR (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018)

/* Threshold = 50% of steady = 0.5*(X/1000*Vbus/Rs)*CUR_FS, Vbus=24, Rs=0.54,
 * CUR_FS=1820 cts/A -> dev = X*4044/100 ADC codes. */
#define THR_DEV(X) ((uint32_t)(X) * 4044u / 100u)
#define X_SMALL 40
#define X_LARGE 320

static long sh(long op, void *a){register long r0 asm("r0")=op;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void puts_(const char *s){sh(SYS_WRITE0,(void*)s);}
static void putu(uint32_t v){char b[12];int i=11;b[i--]=0;if(!v)b[i--]='0';while(v&&i>=0){b[i--]='0'+v%10;v/=10;}puts_(&b[i+1]);}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { [0] = (void (*)(void))STACK_TOP, [1] = reset_handler };

static uint32_t rd_id(void)
{
    ADC_CMDL0 = 5; ADC_CMDH0 = 0; ADC_SWTRIG = 1u;
    while ((ADC_FCTRL0 & 0x1Fu) == 0u) { }
    return ADC_RESFIFO0 & 0xFFFF;
}

/* Apply a d-axis step of magnitude X (per-mille) and time the rise to 50%. */
static uint32_t measure(int X)
{
    SM_VAL3(0) = (uint16_t)(250 + X/2); SM_VAL2(0) = (uint16_t)(-(250 + X/2));
    SM_VAL3(1) = (uint16_t)(250 - X/4); SM_VAL2(1) = (uint16_t)(-(250 - X/4));
    SM_VAL3(2) = (uint16_t)(250 - X/4); SM_VAL2(2) = (uint16_t)(-(250 - X/4));
    SYST_CVR = 0;                          /* restart the down-counter */
    MCTRL = RUN012 | LDOK;                 /* commit + run -> rise from id=0 */
    uint32_t thr = ADC_MID + THR_DEV(X), guard = 0;
    while (rd_id() < thr) { if (++guard > 50000000u) break; }
    return (SYST_RVR - SYST_CVR) & 0xFFFFFF;
}

void reset_handler(void)
{
    ADC_CTRL = 1u | 8u; ADC_CMDL0 = 5; ADC_CMDH0 = 0; ADC_TCTRL0 = (1u << 24);
    for (int s = 0; s < 3; s++) { SM_INIT(s) = (uint16_t)(-500); SM_VAL1(s) = 499; }
    SYST_RVR = 0xFFFFFF; SYST_CSR = 5;     /* enable, core clock, no interrupt */

    uint32_t ts = measure(X_SMALL);
    MCTRL = 0;                             /* free-wheel: id -> 0 */
    for (volatile int d = 0; d < 2000000; d++) { }
    uint32_t tl = measure(X_LARGE);

    puts_("SAT ts="); putu(ts); puts_(" tl="); putu(tl); puts_("\r\n");
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) { }
}
