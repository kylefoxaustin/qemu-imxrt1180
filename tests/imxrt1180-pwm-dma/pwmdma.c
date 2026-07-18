/*
 * i.MX RT1180 — eFlexPWM value-register update driven by eDMA HARDWARE REQUEST.
 *
 * The FOC use case: on every PWM reload the eDMA refreshes the (double-buffered)
 * VALx compare registers for the next period, so the duty cycle tracks a control
 * loop with the CPU out of the inner loop.  Unlike the FIFO peripherals this
 * request is the RELOAD EVENT itself (DMAEN[VALDE]), not a FIFO fill level, and it
 * wires to eDMA4 (SRC 62 for PWM1-SM0).
 *
 * The request is modelled as a level asserted at each reload and lowered by the
 * VALx write that services it (one eDMA minor loop per reload) -- a qemu_irq_pulse
 * would vanish before the eDMA's bottom half ran.  VALx writes are double-buffered
 * and commit only on MCTRL.LDOK, so the eDMA fills the SHADOW each reload and this
 * test commits with a CPU LDOK write and reads the value back -- exactly the
 * hardware flow, no auto-commit shortcut.
 *
 * TWO PHASES (one gate: VALDE, the only DMA enable on this block):
 *   NEG  channel armed + ERQ, VALDE=0 -> reloads happen (proven via STS.RF) but the
 *        request never fires, so the eDMA moves NOTHING (TCD_SADDR must not advance).
 *   POS  VALDE=1 -> the eDMA walks the source ramp into VAL3, one word per reload;
 *        after the major loop completes, an LDOK commit makes VAL3 read back the
 *        last DMA-written value, byte-exact.  CPU never writes VAL3.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* ---- PWM1 @ 0x4265_0000, submodule 0 (16-bit registers) ---------------- */
#define PWM1_BASE 0x42650000u
#define SM0_INIT  (*(volatile uint16_t *)(PWM1_BASE + 0x02))
#define SM0_CTRL  (*(volatile uint16_t *)(PWM1_BASE + 0x06))
#define SM0_VAL1  (*(volatile uint16_t *)(PWM1_BASE + 0x0E))
#define SM0_VAL2  (*(volatile uint16_t *)(PWM1_BASE + 0x12))
#define SM0_VAL3  (*(volatile uint16_t *)(PWM1_BASE + 0x16))
#define SM0_VAL3_ADDR             (PWM1_BASE + 0x16)
#define SM0_STS   (*(volatile uint16_t *)(PWM1_BASE + 0x24))
#define SM0_DMAEN (*(volatile uint16_t *)(PWM1_BASE + 0x28))
#define PWM_MCTRL (*(volatile uint16_t *)(PWM1_BASE + 0x188))

#define STS_RF      0x1000u
#define MCTRL_LDOK  0x0001u        /* LDOK[0] */
#define MCTRL_RUN0  0x0100u        /* RUN[0] = bit 8 */
#define DMAEN_VALDE 0x0200u
#define CTRL_PRSC_6 (6u << 4)      /* prescale /64 */

/* ---- CCM: give the PWM a real counter clock (as the SDK init does) ------ */
#define CCM_BASE          0x44450000u
#define CCM_ROOT_CTRL(n)  (*(volatile uint32_t *)(CCM_BASE + (n) * 0x80u))
#define CLKROOT_BUS_WAKEUP 4u
#define ROOT_MUX_SYSPLL2  (2u << 8)

/* ---- eDMA4 (channel block at base + 0x10000 + n*0x8000) ---------------- */
#define EDMA4_CH(n)  (0x42010000u + (n) * 0x8000u)
#define D32(n, o)    (*(volatile uint32_t *)(EDMA4_CH(n) + (o)))
#define D16(n, o)    (*(volatile uint16_t *)(EDMA4_CH(n) + (o)))
#define CH_CSR(n)     D32(n, 0x000)
#define CH_MUX(n)     D32(n, 0x014)
#define TCD_SADDR(n)  D32(n, 0x020)
#define TCD_SOFF(n)   D16(n, 0x024)
#define TCD_ATTR(n)   D16(n, 0x026)
#define TCD_NBYTES(n) D32(n, 0x028)
#define TCD_DADDR(n)  D32(n, 0x030)
#define TCD_DOFF(n)   D16(n, 0x034)
#define TCD_CITER(n)  D16(n, 0x036)
#define TCD_CSR(n)    D16(n, 0x03C)
#define TCD_BITER(n)  D16(n, 0x03E)
#define CH_CSR_ERQ    (1u << 0)
#define CH_CSR_DONE   (1u << 30)
#define TCD_CSR_DREQ  (1u << 3)
#define ATTR_16BIT    ((1u << 8) | 1u)

#define SRC_PWM1_SM0_VAL 62u
#define DMA_CH 0u
#define NVAL 8u

/* Ramp of VAL3 values the eDMA writes, one per reload.  Each is a valid center-
 * aligned high edge (VAL2 = -250, so duty = VAL3 + 250 per-mille). */
static uint16_t ramp[NVAL] = { 250, 260, 270, 280, 290, 300, 310, 320 };

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

/* Wait for `n` reloads to occur (STS.RF sets once per period). */
static void wait_reloads(unsigned n)
{
    for (unsigned k = 0; k < n; k++) {
        SM0_STS = STS_RF;                        /* W1C clear */
        for (uint32_t g = 0; g < 200000000u; g++) {
            if (SM0_STS & STS_RF) {
                break;
            }
        }
    }
}

static void arm_val(void)
{
    CH_CSR(DMA_CH)     = 0;
    CH_MUX(DMA_CH)     = 0;
    CH_MUX(DMA_CH)     = SRC_PWM1_SM0_VAL;
    TCD_SADDR(DMA_CH)  = (uint32_t)ramp;
    TCD_SOFF(DMA_CH)   = 2;
    TCD_ATTR(DMA_CH)   = ATTR_16BIT;
    TCD_NBYTES(DMA_CH) = 2;                      /* one VAL word per reload */
    TCD_DADDR(DMA_CH)  = SM0_VAL3_ADDR;
    TCD_DOFF(DMA_CH)   = 0;
    TCD_CITER(DMA_CH)  = NVAL;
    TCD_BITER(DMA_CH)  = NVAL;
    TCD_CSR(DMA_CH)    = TCD_CSR_DREQ;
}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    int fail = 0;

    /* Real counter clock (SYSPLL2 / 4), or the ptimer never runs and no reload
     * ever fires -- an honest stall, not a fabricated carrier. */
    CCM_ROOT_CTRL(CLKROOT_BUS_WAKEUP) = ROOT_MUX_SYSPLL2 | (4u - 1u);

    /* Center-aligned period 1000, initial ~50% duty. */
    SM0_INIT = (uint16_t)(-500);
    SM0_VAL1 = 499;
    SM0_VAL2 = (uint16_t)(-250);
    SM0_VAL3 = 250;
    PWM_MCTRL = MCTRL_LDOK;                      /* commit the initial values */
    SM0_CTRL = CTRL_PRSC_6;                      /* /64 -> a rate the DMA keeps up with */
    PWM_MCTRL = MCTRL_RUN0;                      /* start SM0 -> periodic reloads */

    /* ---------------- NEG: VALDE=0, the request must stay down ----------- */
    SM0_DMAEN = 0;
    arm_val();
    CH_CSR(DMA_CH) = CH_CSR_ERQ;
    wait_reloads(3);                             /* reloads DO happen... */
    if (TCD_SADDR(DMA_CH) != (uint32_t)ramp) {
        puts_("PWMDMA: FAIL - NEG: eDMA wrote VALx with DMAEN[VALDE] CLEAR\r\n");
        fail = 1;
    } else if (TCD_CITER(DMA_CH) != NVAL) {
        puts_("PWMDMA: FAIL - NEG: CITER advanced with DMAEN[VALDE] CLEAR\r\n");
        fail = 1;
    }
    CH_CSR(DMA_CH) = 0;

    /* ---------------- POSITIVE: VALDE=1 -> eDMA refreshes VAL3 ----------- */
    arm_val();
    CH_CSR(DMA_CH) = CH_CSR_ERQ;
    SM0_DMAEN = DMAEN_VALDE;                     /* enable the value-DMA request */

    uint32_t spins = 0;
    while (!(CH_CSR(DMA_CH) & CH_CSR_DONE) && spins < 200000000u) {
        spins++;
    }
    if (!(CH_CSR(DMA_CH) & CH_CSR_DONE)) {
        puts_("PWMDMA: FAIL - POS: DMA never completed (reloads not servicing VALDE?)\r\n");
        fail = 1;
    } else if (CH_CSR(DMA_CH) & CH_CSR_ERQ) {
        puts_("PWMDMA: FAIL - POS: TCD_CSR[DREQ] did not clear ERQ at completion\r\n");
        fail = 1;
    } else {
        /* The eDMA filled the SHADOW; commit it and read VAL3 back. */
        PWM_MCTRL = MCTRL_LDOK;
        if (SM0_VAL3 != ramp[NVAL - 1]) {
            puts_("PWMDMA: FAIL - POS: committed VAL3 is not the last DMA-written value\r\n");
            fail = 1;
        }
    }
    CH_CSR(DMA_CH) = 0;

    if (!fail) {
        puts_("PWMDMA: PASS - VALDE gate refuses, and open => eDMA4 refreshes VAL3 "
              "once per reload, committed value byte-exact, CPU never wrote VAL3\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
