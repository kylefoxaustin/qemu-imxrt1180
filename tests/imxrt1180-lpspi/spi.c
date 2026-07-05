/*
 * LPSPI controller test (Cortex-M33).
 *
 * Issues a READ JEDEC ID (0x9F) to a serial-flash on LPSPI4 and prints the
 * 3 returned ID bytes.  Attach the flash to the default (last) SSI bus:
 *   -device at25df321a
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* LPSPI4 @ 0x4256_0000 (owns the default SSI bus). */
#define LPSPI_BASE 0x42560000u
#define CR    (*(volatile uint32_t *)(LPSPI_BASE + 0x10))
#define SR    (*(volatile uint32_t *)(LPSPI_BASE + 0x14))
#define CFGR1 (*(volatile uint32_t *)(LPSPI_BASE + 0x24))
#define TCR   (*(volatile uint32_t *)(LPSPI_BASE + 0x60))
#define TDR   (*(volatile uint32_t *)(LPSPI_BASE + 0x64))
#define RDR   (*(volatile uint32_t *)(LPSPI_BASE + 0x74))

#define CR_MEN 0x1u
#define CR_RST 0x2u
#define TCR_FRAMESZ8 0x7u          /* FRAMESZ = 8 bits - 1 */
#define TCR_CONT     0x200000u
#define CMD_READ_ID  0x9Fu

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
static void putbyte(const char *l, uint32_t b)
{
    static const char hx[] = "0123456789abcdef";
    char buf[12] = { l[0], l[1], '=', '0', 'x', hx[(b >> 4) & 0xf], hx[b & 0xf],
                     '\r', '\n', 0 };
    puts_(buf);
}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    CR = CR_RST; CR = 0;
    CFGR1 = 0x1;                    /* MASTER mode */
    CR = CR_MEN;

    TCR = TCR_FRAMESZ8 | TCR_CONT;
    TDR = CMD_READ_ID;             /* command byte */
    TDR = 0x00;                    /* clock out ID[0] */
    TDR = 0x00;                    /* clock out ID[1] */
    TCR = TCR_FRAMESZ8;            /* last frame -> CONT cleared, CS drops */
    TDR = 0x00;                    /* clock out ID[2] */

    uint32_t d0 = RDR, id0 = RDR, id1 = RDR, id2 = RDR;
    (void)d0;
    putbyte("m0", id0); putbyte("d1", id1); putbyte("d2", id2);
    if ((id0 & 0xFF) != 0x00 && (id0 & 0xFF) != 0xFF) {
        puts_("LPSPI: PASS - flash JEDEC ID read over SPI\r\n");
    } else {
        puts_("LPSPI: FAIL - no valid JEDEC ID (flash did not respond)\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
