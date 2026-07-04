/*
 * Bare-metal LPUART1 console test for the i.MX RT1180 (Cortex-M33).
 *
 * Enables the LPUART1 transmitter and prints a banner byte-by-byte through the
 * real LPUART DATA register (polling STAT.TDRE), then exits via semihosting so
 * the QEMU run terminates.  Proves the imxrt1180-lpuart model's TX path and the
 * SoC mapping at 0x44380000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART1_BASE  0x44380000u
#define LPUART_STAT   (*(volatile uint32_t *)(LPUART1_BASE + 0x14))
#define LPUART_CTRL   (*(volatile uint32_t *)(LPUART1_BASE + 0x18))
#define LPUART_DATA   (*(volatile uint32_t *)(LPUART1_BASE + 0x1C))
#define CTRL_TE       (1u << 19)
#define STAT_TDRE     (1u << 23)

#define STACK_TOP 0x20020000u   /* top of DTCM */

void reset_handler(void);

__attribute__((section(".vectors"), used))
void (* const vector_table[])(void) = {
    (void (*)(void))STACK_TOP,
    reset_handler,
};

static void uart_putc(char c)
{
    while (!(LPUART_STAT & STAT_TDRE)) {
    }
    LPUART_DATA = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

static void semihost_exit(void)
{
    register long r0 asm("r0") = 0x18;       /* SYS_EXIT */
    register long r1 asm("r1") = 0x20026;    /* ApplicationExit */
    asm volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
}

void reset_handler(void)
{
    LPUART_CTRL = CTRL_TE;   /* enable transmitter */
    uart_puts("\r\n=== i.MX RT1180 LPUART1 console alive (Cortex-M33)! ===\r\n");
    semihost_exit();
    for (;;) {
    }
}
