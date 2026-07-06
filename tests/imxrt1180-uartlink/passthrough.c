/*
 * i.MX RT1180 UART passthrough firmware (Cortex-M33) — persistent b2b echo.
 *
 * A raw echo on LPUART2 (the b2b link port, serial_hd(1)): every byte received
 * is sent straight back, from the RX interrupt handler, so the main loop can
 * sit in WFI (idle CPU — friendly to a board farm running many instances).
 * Prints one banner via semihosting, then loops forever and NEVER calls
 * SYS_EXIT — so QEMU stays alive for a persistent farm (holobench) to hold over
 * QMP.  The peer/coordinator drives the protocol; this side is protocol-agnostic.
 * (The self-testing uartlink.elf, which exits after PASS, is the CI counterpart.)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define STACK_TOP  0x20020000u

#define LPUART2_BASE 0x44390000u
#define LPUART_STAT  (*(volatile uint32_t *)(LPUART2_BASE + 0x14))
#define LPUART_CTRL  (*(volatile uint32_t *)(LPUART2_BASE + 0x18))
#define LPUART_DATA  (*(volatile uint32_t *)(LPUART2_BASE + 0x1C))
#define CTRL_TE   0x00080000u
#define CTRL_RE   0x00040000u
#define CTRL_RIE  0x00200000u   /* RX-data-full interrupt enable */
#define STAT_RDRF 0x00200000u
#define STAT_TDRE 0x00800000u

#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define LPUART2_IRQ 20

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void reset_handler(void);
void lpuart2_isr(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0]                 = (void (*)(void))STACK_TOP,
    [1]                 = reset_handler,
    [16 + LPUART2_IRQ]  = lpuart2_isr,
};

/* RX interrupt: echo each received byte straight back. */
void lpuart2_isr(void)
{
    while (LPUART_STAT & STAT_RDRF) {
        uint8_t b = (uint8_t)(LPUART_DATA & 0xFF);   /* read clears RDRF */
        while (!(LPUART_STAT & STAT_TDRE)) {
        }
        LPUART_DATA = b;
    }
}

void reset_handler(void)
{
    LPUART_CTRL = CTRL_TE | CTRL_RE | CTRL_RIE;   /* TX + RX + RX interrupt */
    NVIC_ISER0 = (1u << LPUART2_IRQ);             /* enable LPUART2 IRQ (20) */
    sh(SYS_WRITE0, (void *)"RT1180 PASSTHROUGH ready\r\n");

    for (;;) {
        asm volatile("wfi");    /* idle; the RX ISR does the echo. Never exits. */
    }
}
