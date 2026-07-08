/*
 * QuadTimer (TMR1) test (Cortex-M33).
 *
 * Runs TMR1 channel 0 as a periodic modulo timer: counts the prescaled IP-bus
 * clock (CTRL.CM=1, PCS=0) up to COMP1, sets SCTRL.TCF and raises IRQ 0 (via
 * SCTRL.TCFIE), and reloads.  Verifies the ISR fires repeatedly and the live
 * counter (CNTR) advances.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* TMR1 @ 0x4269_0000, channel 0 (16-bit registers). */
#define TMR1 0x42690000u
#define CH0_COMP1  (*(volatile uint16_t *)(TMR1 + 0x00))
#define CH0_LOAD   (*(volatile uint16_t *)(TMR1 + 0x06))
#define CH0_CNTR   (*(volatile uint16_t *)(TMR1 + 0x0A))
#define CH0_CTRL   (*(volatile uint16_t *)(TMR1 + 0x0C))
#define CH0_SCTRL  (*(volatile uint16_t *)(TMR1 + 0x0E))
#define TMR_ENBL   (*(volatile uint16_t *)(TMR1 + 0x1E))

#define CTRL_CM_PRIMARY 0x2000u   /* CM=1: count rising edge of primary source */
#define SCTRL_TCF       0x8000u
#define SCTRL_TCFIE     0x4000u

#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define TMR1_IRQ   0

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

static volatile int ticks;

void reset_handler(void);
void tmr1_isr(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0]              = (void (*)(void))STACK_TOP,
    [1]              = reset_handler,
    [16 + TMR1_IRQ]  = tmr1_isr,
};

void tmr1_isr(void)
{
    CH0_SCTRL = SCTRL_TCFIE;      /* clear TCF (write 0 to it), keep TCFIE */
    ticks++;
}

void reset_handler(void)
{
    CH0_LOAD  = 0;
    CH0_COMP1 = 5000;                       /* modulo */
    CH0_SCTRL = SCTRL_TCFIE;                /* compare interrupt enable */
    CH0_CTRL  = CTRL_CM_PRIMARY;            /* PCS=0 (IP bus /1), CM=1 */
    NVIC_ISER0 = (1u << TMR1_IRQ);
    TMR_ENBL  = 0x1;                        /* enable channel 0 -> starts */

    uint16_t c1 = CH0_CNTR;
    while (ticks < 3) {                     /* wait for periodic compares */
    }
    uint16_t c2 = CH0_CNTR;

    if (ticks >= 3 && c1 != c2) {
        puts_("TMR: PASS - periodic compare IRQ fires + counter runs\r\n");
    } else {
        puts_("TMR: FAIL - QuadTimer did not tick / counter stuck\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
