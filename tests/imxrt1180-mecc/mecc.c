/*
 * Bare-metal MECC value-golden for the i.MX RT1180 (Cortex-M33).
 *
 * Exercises MECC2 fronting OCRAM2 the way the stock fsl_mecc single-error demo
 * does, and asserts the values that demo asserts -- fast and mutation-proof,
 * independent of the SDK build:
 *   1. ECC write/read round-trip is transparent.
 *   2. A single-bit error injected via ERR_DATA_INJ_LOW0 is CORRECTED on read
 *      (the read returns the original data).
 *   3. ERR_STATUS.SINGLE_ERR0 is set, and the SINGLE_ERR info registers report
 *      the errored address (0x20), the RAW errored data (orig ^ injected bit),
 *      and the one-hot bit position.
 *
 * Mutation: break the correction (return raw instead of corrected) or the
 * status/info, and the matching check FAILs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define ADP_Stopped_ApplicationExit 0x20026u

#define STACK_TOP  0x20020000u   /* top of DTCM */

#define OCRAM2_BASE   0x20500000u
#define TEST_OFFSET   0x20u
#define MECC2_BASE    0x42930000u
#define MECC_ERR_STATUS        (*(volatile uint32_t *)(MECC2_BASE + 0x00))
#define MECC_ERR_DATA_INJ_LOW0 (*(volatile uint32_t *)(MECC2_BASE + 0x0C))
#define MECC_SE_ADDR_ECC0      (*(volatile uint32_t *)(MECC2_BASE + 0x3C))
#define MECC_SE_DATA_LOW0      (*(volatile uint32_t *)(MECC2_BASE + 0x40))
#define MECC_SE_DATA_HIGH0     (*(volatile uint32_t *)(MECC2_BASE + 0x44))
#define MECC_SE_POS_LOW0       (*(volatile uint32_t *)(MECC2_BASE + 0x48))
#define MECC_PIPE_ECC_EN       (*(volatile uint32_t *)(MECC2_BASE + 0x100))
#define PIPE_ECC_EN_ECC_EN     (1u << 4)
#define ERR_STATUS_SINGLE_ERR0 (1u << 0)

#define TESTVAL   0x1122334444332211ull
#define BITPOS    2u
#define RAW_LOW   (0x44332211u ^ (1u << BITPOS))   /* 0x44332215 */

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
static void done(int ok)
{
    puts_(ok ? "MECC: PASS - single-bit error corrected on read; status+info correct\r\n"
             : "MECC: FAIL\r\n");
    sh(SYS_EXIT, (void *)ADP_Stopped_ApplicationExit);
    for (;;) {
    }
}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vector_table[])(void) = {
    (void (*)(void))STACK_TOP,
    reset_handler,
};

void reset_handler(void)
{
    volatile uint64_t *p = (volatile uint64_t *)(OCRAM2_BASE + TEST_OFFSET);

    /* Enable ECC on OCRAM2. */
    MECC_PIPE_ECC_EN = PIPE_ECC_EN_ECC_EN;

    /* (1) Transparent round-trip. */
    *p = TESTVAL;
    if (*p != TESTVAL) {
        puts_("round-trip mismatch\r\n"); done(0);
    }

    /* (2) Arm a single-bit injection (bank 0, low bit BITPOS), rewrite, read. */
    MECC_ERR_DATA_INJ_LOW0 = 1u << BITPOS;
    *p = TESTVAL;                       /* poisoned write */
    uint64_t r = *p;                    /* ECC-corrected read */
    if (r != TESTVAL) {
        puts_("single-bit error was NOT corrected\r\n"); done(0);
    }

    /* (3) Status + info. */
    if (!(MECC_ERR_STATUS & ERR_STATUS_SINGLE_ERR0)) {
        puts_("SINGLE_ERR0 status not set\r\n"); done(0);
    }
    if (((MECC_SE_ADDR_ECC0 >> 8) & 0x7FFFFu) != TEST_OFFSET) {
        puts_("single-error address wrong\r\n"); done(0);
    }
    if (MECC_SE_DATA_LOW0 != RAW_LOW) {
        puts_("single-error raw low data wrong\r\n"); done(0);
    }
    if (MECC_SE_DATA_HIGH0 != 0x11223344u) {
        puts_("single-error raw high data wrong\r\n"); done(0);
    }
    if (MECC_SE_POS_LOW0 != (1u << BITPOS)) {   /* one-hot position */
        puts_("single-error bit position wrong\r\n"); done(0);
    }

    done(1);
}
