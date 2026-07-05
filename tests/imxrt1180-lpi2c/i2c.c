/*
 * LPI2C controller test (Cortex-M33).
 *
 * Reads the temperature register of a TMP105 sensor on LPI2C4, then probes a
 * vacant address to confirm NACK detection.  A bus-less -device attaches to the
 * default I2C bus, which QEMU picks as the last-registered one = LPI2C4's:
 *   -device tmp105,address=0x48,temperature=25000
 * (25000 m°C -> raw 0x1900, so the high byte reads 0x19).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* LPI2C4 @ 0x4254_0000 (owns the default I2C bus). */
#define LPI2C_BASE 0x42540000u
#define MCR  (*(volatile uint32_t *)(LPI2C_BASE + 0x10))
#define MSR  (*(volatile uint32_t *)(LPI2C_BASE + 0x14))
#define MTDR (*(volatile uint32_t *)(LPI2C_BASE + 0x60))
#define MRDR (*(volatile uint32_t *)(LPI2C_BASE + 0x70))

#define MSR_NDF    0x400u
#define MRDR_EMPTY 0x4000u

#define CMD_START 0x400u   /* (0b100 << 8) | addr */
#define CMD_TX    0x000u
#define CMD_RX    0x100u   /* (0b001 << 8) | (n-1) */
#define CMD_STOP  0x200u

#define TMP105_ADDR 0x48u

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

static void putbyte(const char *label, uint32_t b)
{
    static const char hx[] = "0123456789abcdef";
    char buf[16] = { label[0], label[1], '=', '0', 'x',
                     hx[(b >> 4) & 0xf], hx[b & 0xf], '\r', '\n', 0 };
    puts_(buf);
}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    MCR = 0x1;   /* MEN: enable controller */

    /* --- read TMP105 register 0x00 (temperature), 2 bytes --- */
    MTDR = CMD_START | ((TMP105_ADDR << 1) | 0u);  /* START + addr (write) */
    MTDR = CMD_TX    | 0x00u;                       /* pointer = 0 (temp)   */
    MTDR = CMD_START | ((TMP105_ADDR << 1) | 1u);  /* repeated START (read)*/
    MTDR = CMD_RX    | 1u;                          /* receive 2 bytes      */
    MTDR = CMD_STOP;

    uint32_t hi = MRDR, lo = MRDR;
    putbyte("hi", hi); putbyte("lo", lo);
    if ((MSR & MSR_NDF) || (hi & MRDR_EMPTY) || (lo & MRDR_EMPTY)) {
        puts_("LPI2C: FAIL - TMP105 read did not ACK / no data\r\n");
    } else {
        puts_("LPI2C: PASS - TMP105 register read over I2C (ACK + 2 bytes)\r\n");
        if ((hi & 0xFF) == 0x19) {
            puts_("LPI2C:   value 0x1900 = 25C matches the set temperature\r\n");
        }
    }

    /* --- probe a vacant address: expect NACK (NDF) --- */
    MSR = 0xFF00;                                   /* clear sticky flags   */
    MTDR = CMD_START | ((0x50u << 1) | 0u);         /* nobody at 0x50       */
    MTDR = CMD_STOP;
    if (MSR & MSR_NDF) {
        puts_("LPI2C: PASS - NACK detected for absent device (NDF)\r\n");
    } else {
        puts_("LPI2C: FAIL - no NACK for absent device\r\n");
    }

    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
