/*
 * i.MX RT1180 — WM8962 audio codec control-plane test (Cortex-M33).
 *
 * The WM8962 is the EVK's audio codec on LPI2C2 at 0x1A.  The stock SAI codec demo
 * calls CODEC_Init, which does real I2C register reads/writes over this bus (no
 * soft cache) and assert(false)s if the codec does not answer.  This test drives
 * the codec exactly as that driver does -- a 2-byte register address followed by a
 * 16-bit big-endian value -- and proves:
 *
 *   1. A written register READS BACK the written value (the driver read-modify-
 *      writes; a codec that dropped writes would corrupt every RMW).
 *   2. The write-sequencer BUSY register (0x5D bit0) reads 0 even after a driver
 *      writes it 1 -- WM8962_StartSequence polls this and hangs otherwise.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* LPI2C2 @ 0x4435_0000 (the EVK codec/sensor bus). */
#define LPI2C2 0x44350000u
#define MCR   (*(volatile uint32_t *)(LPI2C2 + 0x10))
#define MSR   (*(volatile uint32_t *)(LPI2C2 + 0x14))
#define MTDR  (*(volatile uint32_t *)(LPI2C2 + 0x60))
#define MRDR  (*(volatile uint32_t *)(LPI2C2 + 0x70))

#define MCR_MEN    0x1u
#define MSR_NDF    0x400u
#define MRDR_EMPTY 0x4000u

#define CMD_START 0x400u
#define CMD_TX    0x000u
#define CMD_RX    0x100u
#define CMD_STOP  0x200u

#define WM8962_ADDR 0x1Au

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
static void die(const char *s) { puts_(s); sh(SYS_EXIT, (void *)0x20026u); }

/* WM8962 register write: START, addr(W), reg[hi,lo], val[hi,lo], STOP. */
static void wm_write(uint16_t reg, uint16_t val)
{
    MTDR = CMD_START | ((WM8962_ADDR << 1) | 0u);
    MTDR = CMD_TX | (reg >> 8);
    MTDR = CMD_TX | (reg & 0xFF);
    MTDR = CMD_TX | (val >> 8);
    MTDR = CMD_TX | (val & 0xFF);
    MTDR = CMD_STOP;
}

/* WM8962 register read: START, addr(W), reg[hi,lo], rSTART, addr(R), recv 2, STOP. */
static int wm_read(uint16_t reg, uint16_t *out)
{
    MTDR = CMD_START | ((WM8962_ADDR << 1) | 0u);
    MTDR = CMD_TX | (reg >> 8);
    MTDR = CMD_TX | (reg & 0xFF);
    MTDR = CMD_START | ((WM8962_ADDR << 1) | 1u);
    MTDR = CMD_RX | 1u;                 /* receive 2 bytes */
    MTDR = CMD_STOP;

    uint32_t hi = MRDR, lo = MRDR;
    if ((MSR & MSR_NDF) || (hi & MRDR_EMPTY) || (lo & MRDR_EMPTY)) {
        return -1;
    }
    *out = (uint16_t)(((hi & 0xFF) << 8) | (lo & 0xFF));
    return 0;
}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    uint16_t v;

    MCR = MCR_MEN;

    /* (0) The codec must ACK its address at all. */
    if (wm_read(0x00, &v) != 0) {
        die("WM8962: FAIL - codec did not ACK on LPI2C2 @0x1A\r\n");
    }

    /* (1) A written register reads back the written value. */
    wm_write(0x02, 0xABCD);
    if (wm_read(0x02, &v) != 0) {
        die("WM8962: FAIL - read after write did not complete\r\n");
    }
    if (v != 0xABCD) {
        die("WM8962: FAIL - register did not read back the written value\r\n");
    }

    /* (2) A DIFFERENT register is independent (not a single latched value). */
    wm_write(0x08, 0x1234);
    if (wm_read(0x02, &v) != 0 || v != 0xABCD) {
        die("WM8962: FAIL - a second write clobbered the first register\r\n");
    }

    /* (3) The write-sequencer BUSY bit (0x5D bit0) reads 0 even after set to 1,
     *     so WM8962_StartSequence's poll exits instead of hanging. */
    wm_write(0x5D, 0x0001);
    if (wm_read(0x5D, &v) != 0) {
        die("WM8962: FAIL - could not read the write-sequencer status\r\n");
    }
    if (v & 0x0001) {
        die("WM8962: FAIL - 0x5D BUSY bit stuck set; StartSequence would hang\r\n");
    }

    puts_("WM8962: PASS - codec ACKs, registers round-trip, and the write-sequencer "
          "BUSY poll clears (CODEC_Init can complete)\r\n");
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
