/*
 * Inter-core MU test — Cortex-M7 (MUB) side.
 *
 * Enables the MU1 receive interrupt (MUB.RCR.RIE0) + IRQ21 in its own NVIC,
 * then WFI-sleeps.  When the M33's message lands in MUB.RR[0] the receive IRQ
 * fires; the ISR reads the word and replies word+1 on MUB.TR[0] (-> MUA.RR[0]).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define M7_STACK_TOP 0x30440000u

/* MUB = CM7 side of MU1 @ 0x4423_0000. */
#define MUB_RCR (*(volatile uint32_t *)0x44230128u)   /* RIEn */
#define MUB_TR0 (*(volatile uint32_t *)0x44230200u)
#define MUB_RR0 (*(volatile uint32_t *)0x44230280u)

#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define MU1_IRQ    21

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void m7_reset(void);
void mu_isr(void);

/* Vector table: SP, reset, and the MU1 handler at exception #(16 + IRQ21). */
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0]           = (void (*)(void))M7_STACK_TOP,
    [1]           = m7_reset,
    [16 + MU1_IRQ] = mu_isr,
};

void mu_isr(void)
{
    uint32_t msg = MUB_RR0;     /* read clears RSR.RF0 (the IRQ source) */
    MUB_TR0 = msg + 1u;         /* echo back +1 */
}

void m7_reset(void)
{
    sh(SYS_WRITE0, (void *)"M7:  arming MU1 receive IRQ (RIE0 + NVIC 21)...\r\n");
    MUB_RCR    = 0x1u;                 /* RIE0: IRQ when RR[0] is full */
    NVIC_ISER0 = (1u << MU1_IRQ);      /* enable IRQ21 in the M7 NVIC   */
    for (;;) {
        asm volatile("wfi");
    }
}
