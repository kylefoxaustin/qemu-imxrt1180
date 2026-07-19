/*
 * LPADC A/B-side dual-conversion test (Cortex-M33).
 *
 * The stock mc_pmsm cm7 FOC demo does NOT read phase currents single-ended: it
 * issues ONE DualSingleEndBothSide command per ADC that samples BOTH the A-side
 * and B-side mux of a single channel simultaneously, and reads the two results
 * from RESFIFO[0] (A) and RESFIFO[1] (B) -- Ia on ADC1_A5, Ib on ADC1_B5.  This
 * test drives the virtual PMSM plant to a steady state with DISTINCT, non-zero
 * Ia and Ib, then reads them back through a single dual command and checks each
 * side against a first-principles golden.
 *
 * A stator voltage vector at electrical angle 150 deg aligns the rotor there; at
 * steady state the current is purely resistive (|i| = |v|/Rs) along that axis:
 *   |v| = 1.4965 V (same amplitude as the motor test), Rs = 0.54 ohm
 *   |i| = 2.7713 A
 *   ia = |i| cos(150 deg) = -2.400 A   (A-side ch5 -> RESFIFO[0])
 *   ib = |i| cos( 30 deg) = +2.400 A   (B-side ch5 -> RESFIFO[1])
 * Current-sense scaling: the mc_pmsm driver decodes 32768/(2*(12/11)*8.25) = 1820
 * counts/A, so 2.400 A = 4369 counts.  Ia = 0x8000 - 4369, Ib = 0x8000 + 4369 --
 * equal magnitude, OPPOSITE sign,
 * neither the un-driven mid-scale placeholder.  Rs/Pp/I_MAX are datasheet facts,
 * independent of the plant's code.
 *
 * The duties are the motor test's {250,277,223} permuted for 150 deg:
 *   va = |v| cos150 = -1.296 V -> da = 0.446 -> VAL3 = 223  (SM0)
 *   vb = |v| cos30  = +1.296 V -> db = 0.554 -> VAL3 = 277  (SM1)
 *   vc = |v| cos-90 =  0 V     -> dc = 0.500 -> VAL3 = 250  (SM2)
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
#define MCTRL_LDOK   0x000Fu
#define MCTRL_RUN012 0x0700u

#define EQDC1_LPOS (*(volatile uint16_t *)(0x42710000u + 0x0E))

#define ADC1_BASE 0x42600000u
#define ADC_CTRL     (*(volatile uint32_t *)(ADC1_BASE + 0x10))
#define ADC_SWTRIG   (*(volatile uint32_t *)(ADC1_BASE + 0x34))
#define ADC_TCTRL0   (*(volatile uint32_t *)(ADC1_BASE + 0xA0))
#define ADC_FCTRL0   (*(volatile uint32_t *)(ADC1_BASE + 0xE0))
#define ADC_FCTRL1   (*(volatile uint32_t *)(ADC1_BASE + 0xE4))
#define ADC_CMDL0    (*(volatile uint32_t *)(ADC1_BASE + 0x100))
#define ADC_CMDH0    (*(volatile uint32_t *)(ADC1_BASE + 0x104))
#define ADC_RESFIFO0 (*(volatile uint32_t *)(ADC1_BASE + 0x300))
#define ADC_RESFIFO1 (*(volatile uint32_t *)(ADC1_BASE + 0x304))
#define CTRL_ADCEN   0x1u
#define CTRL_CAL_REQ 0x8u
#define ADC_MID      0x8000

/* CMDL: A-side channel in ADCH[4:0], CTYPE[6:5]=3 => DualSingleEndBothSide. */
#define CMDL_DUAL_CH(ch) ((uint32_t)((ch) & 0x1F) | (3u << 5))

#define I_EXPECT_DI 4369            /* |2.400 A| in ADC counts (mc_pmsm scale) */
#define I_TOL       (I_EXPECT_DI / 20)   /* +/-5% */

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

/* Trigger CMD1 (a dual A/B command on ch5) and return both FIFO results. */
static void read_dual(uint32_t *a_out, uint32_t *b_out)
{
    ADC_CMDL0 = CMDL_DUAL_CH(5);       /* A5 + B5 */
    ADC_CMDH0 = 0;
    ADC_SWTRIG = 1u;
    while ((ADC_FCTRL0 & 0x1Fu) == 0u || (ADC_FCTRL1 & 0x1Fu) == 0u) {
    }
    *a_out = ADC_RESFIFO0 & 0xFFFF;    /* A-side */
    *b_out = ADC_RESFIFO1 & 0xFFFF;    /* B-side */
}

static int near_mag(uint32_t code, int expect_di_signed)
{
    int32_t di = (int32_t)code - ADC_MID;
    int32_t want = expect_di_signed;
    int32_t lo = want - (want < 0 ? -I_TOL : I_TOL);
    int32_t hi = want + (want < 0 ? -I_TOL : I_TOL);
    if (lo > hi) { int32_t t = lo; lo = hi; hi = t; }
    return di >= lo && di <= hi;
}

void reset_handler(void)
{
    int ok = 1;

    ADC_CTRL   = CTRL_ADCEN | CTRL_CAL_REQ;
    ADC_TCTRL0 = (1u << 24);            /* TCMD = command 1, software trigger */

    /* Baseline: rotor idle -> both sides read the mid-scale placeholder. */
    uint32_t a0 = 0, b0 = 0;
    read_dual(&a0, &b0);
    if (a0 != ADC_MID || b0 != ADC_MID) { ok = 0; }

    /* Apply the 150-deg stator vector (period 1000). */
    static const uint16_t val3[3] = { 223, 277, 250 };   /* SM0, SM1, SM2 */
    for (int s = 0; s < 3; s++) {
        SM_INIT(s) = (uint16_t)(-500);
        SM_VAL1(s) = 499;
        SM_VAL2(s) = (uint16_t)(-(int)val3[s]);
        SM_VAL3(s) = val3[s];
    }
    PWM_MCTRL = MCTRL_LDOK;
    PWM_MCTRL = MCTRL_RUN012;

    /* Let the rotor swing to alignment and settle. */
    uint32_t guard = 0;
    while (EQDC1_LPOS == 0) {
        if (++guard > 300000000u) { ok = 0; break; }
    }
    for (volatile int i = 0; i < 40000000; i++) {
    }

    /* Read Ia (A-side) and Ib (B-side) from ONE dual command. */
    uint32_t ia = 0, ib = 0;
    read_dual(&ia, &ib);

    /* A-side must carry Ia (-2.400 A), B-side Ib (+2.400 A): both first-principles. */
    int a_ok = near_mag(ia, -I_EXPECT_DI);    /* below mid-scale */
    int b_ok = near_mag(ib, +I_EXPECT_DI);    /* above mid-scale */
    /* The mux must actually SEPARATE the sides: a model that routed one side to
     * both FIFOs would make these equal (and both fail their opposite-sign golden). */
    int distinct = (ia != ib);
    int not_placeholder = (ia != ADC_MID) && (ib != ADC_MID);

    if (!(a_ok && b_ok && distinct && not_placeholder)) { ok = 0; }

    if (ok) {
        puts_("ADCAB: PASS - dual A/B command splits Ia(A5)->RESFIFO0 and Ib(B5)->RESFIFO1,\r\n");
        puts_("ADCAB: PASS - each side matches the first-principles current golden (+/-2.400 A)\r\n");
    } else if (!distinct || !not_placeholder) {
        puts_("ADCAB: FAIL - A and B sides not separated (mux collapsed or undriven)\r\n");
    } else {
        puts_("ADCAB: FAIL - dual-conversion currents do not match the physics golden\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
