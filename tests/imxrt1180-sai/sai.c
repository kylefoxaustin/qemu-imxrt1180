/*
 * SAI (I2S) bring-up test (Cortex-M33).
 *
 * Exercises the SAI1 transmit init handshake: software reset + FIFO reset both
 * self-clear, the TX enable (TE) sticks, and the transmit FIFO advertises space
 * (FWF) with no error (FEF) — i.e. firmware init settles.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

#define TCSR (*(volatile uint32_t *)(0x443B0000u + 0x08))   /* SAI1 TCSR */

#define CSR_FWF (1u << 17)
#define CSR_FEF (1u << 18)
#define CSR_SR  (1u << 24)
#define CSR_FR  (1u << 25)
#define CSR_EN  (1u << 31)   /* TE */

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
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    TCSR = CSR_SR;               /* software reset (momentary) */
    uint32_t after_sr = TCSR;
    TCSR = CSR_FR;               /* FIFO reset (momentary) */
    uint32_t after_fr = TCSR;
    TCSR = CSR_EN;               /* enable transmitter */
    uint32_t en = TCSR;

    int ok = !(after_sr & CSR_SR) && !(after_fr & CSR_FR) &&
             (en & CSR_EN) && (en & CSR_FWF) && !(en & CSR_FEF);
    if (ok) {
        puts_("SAI: PASS - TX init handshake settles (reset self-clear + FWF)\r\n");
    } else {
        puts_("SAI: FAIL - init handshake did not settle\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
