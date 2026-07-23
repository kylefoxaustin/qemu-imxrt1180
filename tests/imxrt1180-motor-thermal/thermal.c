/*
 * Virtual-motor plant — WINDING THERMAL model value-test (Cortex-M33).
 *
 * Same open-loop drive as tests/imxrt1180-motor (a small fixed stator voltage
 * vector, rotor aligns at electrical 90 deg, a resistive phase current flows),
 * but run with the plant's thermal model ENABLED:
 *
 *   -global imxrt1180-motor.thermal=1
 *   -global imxrt1180-motor.therm-rth-mcw=20000   (R_th = 20 degC/W)
 *   -global imxrt1180-motor.therm-tau-ms=1        (fast tau: reach steady state)
 *
 * The winding heats from its own I^2R loss, Rs rises with the copper tempco, and
 * the phase current DROOPS from the cold Ohm's-law value to a hot steady state.
 *
 * ===================== THE HOT-STEADY-STATE GOLDEN =========================
 * The cold test's voltage vector gives |v| = v_beta = 1.4965 V (v_alpha = 0).
 * At alignment iq -> 0, so the stator is resistive: id = |v|/Rs(T). The winding
 * loss P = 1.5*id^2*Rs = 1.5*|v|^2/Rs drives the steady temperature
 *   T_ss = T_amb + P*R_th   =>   Rs = Rs0*(1 + alpha*(T_ss - T_amb))
 * which is a fixed point with a closed form (quadratic in Rs):
 *   Rs = [ Rs0 + sqrt(Rs0^2 + 6*Rs0*alpha*|v|^2*R_th) ] / 2
 * With Rs0 = 0.54, alpha = 0.00393/degC, |v| = 1.4965 V, R_th = 20 degC/W:
 *   Rs = [0.54 + sqrt(0.2916 + 0.570332)]/2 = 0.73420 ohm   (T_ss ~ 116 degC)
 *   id_hot = |v|/Rs = 2.03827 A ; phase B ib = (sqrt3/2)*id = 1.76519 A
 *   di = ib * 1820.4 cts/A = 3214 counts   (cf. COLD 4369 -> a 26% droop)
 * Rs0/alpha/R_th/|v| are datasheet + test constants, NOT model internals, so
 * this golden is independent of the code it checks. If the model droops to the
 * WRONG value (or not at all), di misses 3214 and the test FAILS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

#define PWM1_BASE 0x42650000u
#define SM_INIT(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x02))
#define SM_VAL1(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x0E))
#define SM_VAL2(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x12))
#define SM_VAL3(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x16))
#define PWM_MCTRL  (*(volatile uint16_t *)(PWM1_BASE + 0x188))
#define MCTRL_LDOK 0x000Fu
#define MCTRL_RUN012 0x0700u

#define EQDC1_LPOS (*(volatile uint16_t *)(0x42710000u + 0x0E))

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

/* Cold golden (thermal off) and hot golden (thermal on, R_th = 20 degC/W). */
#define COLD_DI 4369
#define HOT_DI  3214
#define HOT_TOL (HOT_DI / 20)          /* +/-5% */

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

static void putu(uint32_t v)
{
    char b[12];
    int i = 11;
    b[i--] = 0;
    if (v == 0) { b[i--] = '0'; }
    while (v && i >= 0) { b[i--] = '0' + (v % 10); v /= 10; }
    puts_(&b[i + 1]);
}

void reset_handler(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0] = (void (*)(void))STACK_TOP,
    [1] = reset_handler,
};

static uint32_t read_phaseB(void)
{
    ADC_CMDL0 = 6;
    ADC_CMDH0 = 0;
    ADC_SWTRIG = 1u;
    while ((ADC_FCTRL0 & 0x1Fu) == 0u) {
    }
    return ADC_RESFIFO0 & 0xFFFF;
}

void reset_handler(void)
{
    ADC_CTRL  = CTRL_ADCEN | CTRL_CAL_REQ;
    ADC_CMDL0 = 6;
    ADC_CMDH0 = 0;
    ADC_TCTRL0 = (1u << 24);

    /* Same voltage vector as the cold test: duties 0.500/0.554/0.446. */
    static const uint16_t val3[3] = { 250, 277, 223 };
    for (int s = 0; s < 3; s++) {
        SM_INIT(s) = (uint16_t)(-500);
        SM_VAL1(s) = 499;
        SM_VAL2(s) = (uint16_t)(-(int)val3[s]);
        SM_VAL3(s) = val3[s];
    }
    PWM_MCTRL = MCTRL_LDOK;
    PWM_MCTRL = MCTRL_RUN012;

    /* Wait for the rotor to align (~500 cts). */
    uint16_t pos = 0;
    uint32_t guard = 0;
    while (pos < 200) {
        pos = EQDC1_LPOS;
        if (++guard > 300000000u) { break; }
    }
    /* Let BOTH the mechanical and the (fast-tau) thermal transients settle. */
    for (volatile int i = 0; i < 80000000; i++) {
    }

    uint32_t run_i = read_phaseB();
    int32_t di = (int32_t)run_i - ADC_MID;
    if (di < 0) { di = -di; }

    puts_("THERMAL: measured phase-B di = ");
    putu((uint32_t)di);
    puts_("  (cold golden 4369, hot golden 3214)\r\n");

    int hot_ok  = (di >= HOT_DI - HOT_TOL && di <= HOT_DI + HOT_TOL);
    /* The droop must be real: a hot current must be clearly below the cold one. */
    int drooped = (di < COLD_DI - 4 * HOT_TOL);

    if (hot_ok && drooped) {
        puts_("THERMAL: PASS - phase current drooped to the hot-Rs first-principles golden\r\n");
    } else if (!drooped) {
        puts_("THERMAL: FAIL - current did NOT droop (thermal model inert?)\r\n");
    } else {
        puts_("THERMAL: FAIL - drooped, but not to the closed-form hot golden (3214)\r\n");
    }

    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
