/*
 * LPIT timer test (Cortex-M33).
 *
 * Runs LPIT1 channel 0 as a periodic timer, enables its interrupt (IRQ 15) and
 * verifies the ISR fires repeatedly and the down-counter (CVAL) advances.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* LPIT1 @ 0x442F_0000. */
#define LPIT_BASE 0x442F0000u
#define MCR    (*(volatile uint32_t *)(LPIT_BASE + 0x08))
#define MSR    (*(volatile uint32_t *)(LPIT_BASE + 0x0C))
#define MIER   (*(volatile uint32_t *)(LPIT_BASE + 0x10))
#define SETTEN (*(volatile uint32_t *)(LPIT_BASE + 0x14))
#define TVAL0  (*(volatile uint32_t *)(LPIT_BASE + 0x20))
#define CVAL0  (*(volatile uint32_t *)(LPIT_BASE + 0x24))

#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define LPIT1_IRQ  15

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
void lpit_isr(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0]              = (void (*)(void))STACK_TOP,
    [1]              = reset_handler,
    [16 + LPIT1_IRQ] = lpit_isr,
};

void lpit_isr(void)
{
    MSR = 0x1;      /* W1C the channel-0 timer interrupt flag */
    ticks++;
}

void reset_handler(void)
{
    MCR = 0x1;                        /* M_CEN: enable the module clock */
    TVAL0 = 5000;                     /* period ~208 us at 24 MHz */
    MIER = 0x1;                       /* TIE0: channel-0 interrupt */
    NVIC_ISER0 = (1u << LPIT1_IRQ);   /* enable IRQ 15 */
    SETTEN = 0x1;                     /* start channel 0 */

    uint32_t c1 = CVAL0;
    for (volatile int i = 0; i < 50; i++) {
    }
    uint32_t c2 = CVAL0;

    while (ticks < 3) {               /* wait for a few periodic interrupts */
    }

    if (ticks >= 3 && c1 != c2) {
        puts_("LPIT: PASS - periodic IRQ fires + counter runs\r\n");
    } else {
        puts_("LPIT: FAIL - timer did not tick / counter stuck\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
