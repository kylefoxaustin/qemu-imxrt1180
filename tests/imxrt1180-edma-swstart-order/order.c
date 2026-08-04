/*
 * eDMA4 software-START ordering test — the SDK InitCM7DMA clear-then-poll pattern.
 *
 * The SDK's InitCM7DMA (system_MIMXRT1189_cm33.c, from Prepare_CM7) zeroes the M7
 * TCM with eDMA4 channel 0, then:
 *
 *     TCD.CSR = START ;            // kick the transfer -- now IN FLIGHT
 *     CH_CSR  = 0x40000006 ;       // W1C DONE to drop a STALE flag + disarm ERQ
 *     while ((CH_CSR & DONE) == 0) // poll THIS transfer to completion
 *         ;
 *
 * This is only correct if the transfer is still running while the driver clears
 * DONE.  A model that completes a software START synchronously (or at the next
 * bottom-half, BEFORE the guest's next store) sets DONE first, the W1C wipes the
 * fresh flag, and the poll HANGS FOREVER.  The model must complete the transfer
 * after a virtual-time duration so it is genuinely in flight across the clear --
 * deterministic under -icount (this test runs under it, exactly as the mc_pmsm /
 * LPADC RESFIFO timing invariants do).
 *
 * This test reproduces that exact register order on eDMA4 ch0.  It PASSES only
 * when DONE is observed AFTER the W1C clear AND the payload copied byte-exact.
 * A synchronous or BH-immediate model hangs here (harness reports FAIL on the
 * missing PASS line).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* eDMA4 @0x42000000, channel n at +0x10000 + n*0x8000 (PERI_DMA4.h). ch0: */
#define CH0 0x42010000u
#define R32(o) (*(volatile uint32_t *)(CH0 + (o)))
#define R16(o) (*(volatile uint16_t *)(CH0 + (o)))
#define CH_CSR     R32(0x000)
#define TCD_SADDR  R32(0x020)
#define TCD_SOFF   R16(0x024)
#define TCD_ATTR   R16(0x026)
#define TCD_NBYTES R32(0x028)
#define TCD_SLAST  R32(0x02C)
#define TCD_DADDR  R32(0x030)
#define TCD_DOFF   R16(0x034)
#define TCD_CITER  R16(0x036)
#define TCD_DLAST  R32(0x038)
#define TCD_CSR    R16(0x03C)
#define TCD_BITER  R16(0x03E)
#define CSR_DONE   (1u << 30)

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

/* 32 bytes = four 8-byte elements, like InitCM7DMA's 8-byte-element block copy. */
static volatile uint32_t src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static volatile uint32_t dst[8];

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    for (int i = 0; i < 8; i++) {
        dst[i] = 0xDEADBEEFu;   /* non-zero: "nothing moved" cannot masquerade as 0 */
    }

    TCD_SADDR  = (uint32_t)(uintptr_t)src;
    TCD_DADDR  = (uint32_t)(uintptr_t)dst;
    TCD_SOFF   = 8;
    TCD_DOFF   = 8;
    TCD_ATTR   = (3u << 8) | 3u;   /* SSIZE=DSIZE=3 -> 8-byte elements */
    TCD_NBYTES = 32;
    TCD_SLAST  = 0;
    TCD_DLAST  = 0;
    TCD_CITER  = 1;
    TCD_BITER  = 1;

    /* --- the InitCM7DMA order --- */
    CH_CSR  = 0x7;          /* enable: ERQ|EARQ|EEI                                */
    TCD_CSR = 0x8;          /* DREQ, no START                                     */
    TCD_CSR = 0x9;          /* START|DREQ  -> transfer now IN FLIGHT              */
    CH_CSR  = 0x40000006;   /* W1C DONE (drop stale) + EEI|EARQ, ERQ=0           */

    uint32_t spins = 0;
    while ((CH_CSR & CSR_DONE) == 0) {
        if (++spins >= 200000000u) {   /* bounded: a genuine never-done -> FAIL   */
            puts_("EDMA-SWORDER: FAIL - DONE never set after the W1C clear "
                  "(software START completed before the driver's clear -> hang)\r\n");
            sh(SYS_EXIT, (void *)0x20026u);
        }
    }

    if (dst[0] == 1 && dst[1] == 2 && dst[3] == 4 && dst[7] == 8) {
        puts_("EDMA-SWORDER: PASS - DONE re-set after clear, 32 bytes copied byte-exact\r\n");
    } else {
        puts_("EDMA-SWORDER: FAIL - DONE set but payload wrong\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
