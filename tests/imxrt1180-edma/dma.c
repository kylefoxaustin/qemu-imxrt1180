/*
 * eDMA memory-to-memory test (Cortex-M33).
 *
 * PHASE 1 -- CITER = 1. One TCD_CSR[START], 16 bytes copied, DONE set.
 *
 * PHASE 2 -- CITER = 4, AND THIS IS THE ONE THAT MATTERS.
 *
 *   TCD_CSR[START] IS A *SERVICE REQUEST*. It moves ONE MINOR LOOP (NBYTES) and
 *   decrements CITER by one. A channel with CITER=N needs N of them. RM 5.5.5.2
 *   walks a CITER=2 channel through TWO requests explicitly ("the channel retires,
 *   which concludes ONE ITERATION of the major loop" ... "9. Second hardware
 *   request..."), RM 5.4 says the software START "follows the same basic flow as
 *   peripheral requests", and fsl_edma.h says it outright:
 *   EDMA_TriggerChannelStart() -- "This function starts a MINOR LOOP transfer."
 *
 *   ⚠ THIS TEST USED TO BE PHASE 1 ONLY, AND THAT IS WHY THE MODEL WAS WRONG FOR
 *   MONTHS. At CITER=1, ONE MINOR LOOP *IS* THE WHOLE MAJOR LOOP -- so a model
 *   that (wrongly) drains the entire major loop on one START is BIT-FOR-BIT
 *   INDISTINGUISHABLE from a correct one. Every eDMA test we had used CITER=1.
 *   So did the stock NXP edma4/memory_to_memory example, which deliberately sets
 *   minorLoopBytes = the ENTIRE buffer. A whole corpus of green, structurally
 *   incapable of seeing it.
 *
 *   (mcxn947qemu found it the expensive way, 2026-07-12: their whole-major-loop
 *   START ran the transfer a SECOND time when the stock edma4/channel_link example
 *   issued its second EDMA_TriggerChannelStart -- walked off the end of the linked
 *   channels' buffers, corrupted guest memory, and HARD-FAULTED the CPU. Every
 *   instrument they had said the DMA was fine. It was.)
 *
 *   Phase 2 asserts, after the FIRST of four STARTs: exactly ONE element copied,
 *   the other three still holding a non-zero SENTINEL, CITER == 3, DONE clear, and
 *   START auto-cleared. A whole-major-loop model fails on the very first check.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/*
 * eDMA3 @ 0x4400_0000. CHANNEL n IS AT +0x10000 + n * 0x10000 (PERI_DMA.h
 * DMA_Type: "array offset: 0x10000, array step: 0x10000").
 *
 * ⚠ This test used to say `#define CH0 0x44001000` -- +0x1000, the MCXN947 eDMA's
 * geometry, which the model was adapted from and which nobody re-derived from the
 * RT1180 header. The MODEL had the same wrong map, so THIS TEST PASSED: it was
 * confirming that the model agreed with itself. The stock NXP driver, which takes
 * its addresses from the CMSIS header, HUNG.
 */
#define CH0 0x44010000u
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
#define TCD_START  (1u << 0)

#define SENTINEL   0x5A5A5A5Au

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

/*
 * TCD_CSR[START] is a service request whose minor loop completes ASYNCHRONOUSLY
 * (the model defers it, as silicon runs it in-flight over real bus cycles). So we
 * POLL for completion exactly as the stock fsl_edma driver does, rather than read
 * state back in the instruction after the START -- an instant-completion assumption
 * is precisely the fidelity bug that hung the SDK's InitCM7DMA. The bound turns a
 * genuine never-completes into a loud FAIL instead of a hang.
 */
#define WAIT_BOUND 20000000u
static int wait_done(void)
{
    for (uint32_t i = 0; i < WAIT_BOUND; i++) {
        if (CH_CSR & CSR_DONE) { return 1; }
    }
    return 0;
}
static int wait_citer_reaches(unsigned target)
{
    for (uint32_t i = 0; i < WAIT_BOUND; i++) {
        if ((TCD_CITER & 0x7FFFu) == target) { return 1; }
    }
    return 0;
}

static volatile uint32_t src[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
static volatile uint32_t dst[4];

/* Phase 2. A NON-ZERO sentinel, so "nothing moved" cannot be confused with
 * "zeros were moved over zeros" -- the vacuous pass 95emulator warned about. */
static volatile uint32_t src2[4] = { 0xA1A1A1A1, 0xB2B2B2B2, 0xC3C3C3C3, 0xD4D4D4D4 };
static volatile uint32_t dst2[4];

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    dst[0] = dst[1] = dst[2] = dst[3] = 0;

    TCD_SADDR  = (uint32_t)(uintptr_t)src;
    TCD_DADDR  = (uint32_t)(uintptr_t)dst;
    TCD_SOFF   = 4;            /* 32-bit element stride */
    TCD_DOFF   = 4;
    TCD_ATTR   = (2u << 8) | 2u;   /* SSIZE=DSIZE=2 -> 4-byte elements */
    TCD_NBYTES = 16;          /* one minor loop = 16 bytes */
    TCD_SLAST  = 0;
    TCD_DLAST  = 0;
    TCD_CITER  = 1;
    TCD_BITER  = 1;
    TCD_CSR    = TCD_START;   /* trigger the transfer */

    int ok = wait_done() &&
             dst[0] == 0x11111111 && dst[1] == 0x22222222 &&
             dst[2] == 0x33333333 && dst[3] == 0x44444444;
    if (!ok) {
        puts_("eDMA: FAIL - phase1: CITER=1 transfer did not complete correctly\r\n");
    }

    /* ---- PHASE 2: CITER = 4. One START must buy ONE minor loop. ---------- */
    for (int i = 0; i < 4; i++) {
        dst2[i] = SENTINEL;
    }
    CH_CSR     = 0;                       /* clear DONE from phase 1 */
    TCD_SADDR  = (uint32_t)(uintptr_t)src2;
    TCD_DADDR  = (uint32_t)(uintptr_t)dst2;
    TCD_SOFF   = 4;
    TCD_DOFF   = 4;
    TCD_ATTR   = (2u << 8) | 2u;          /* SSIZE=DSIZE=2 -> 4-byte elements */
    TCD_NBYTES = 4;                       /* ONE minor loop = ONE 32-bit word */
    TCD_SLAST  = 0;
    TCD_DLAST  = 0;
    TCD_CITER  = 4;                       /* FOUR minor loops => FOUR requests */
    TCD_BITER  = 4;

    TCD_CSR = TCD_START;                  /* request #1 of 4 */

    /* Exactly ONE minor loop must run: wait for CITER to fall from 4 to 3, no
     * further. (A whole-major-loop model would blow straight past to 0.) */
    if (ok && !wait_citer_reaches(3u)) {
        puts_("eDMA: FAIL - phase2: first START did not run one minor loop "
              "(CITER never reached 3)\r\n");
        ok = 0;
    }
    if (ok && dst2[0] != 0xA1A1A1A1u) {
        puts_("eDMA: FAIL - phase2: first START moved nothing\r\n");
        ok = 0;
    }
    if (ok && (dst2[1] != SENTINEL || dst2[2] != SENTINEL || dst2[3] != SENTINEL)) {
        puts_("eDMA: FAIL - phase2: ONE START ran MORE THAN ONE MINOR LOOP "
              "(TCD_CSR[START] is a service request, not 'run the major loop')\r\n");
        ok = 0;
    }
    if (ok && (TCD_CITER & 0x7FFFu) != 3u) {
        puts_("eDMA: FAIL - phase2: CITER did not decrement by exactly one\r\n");
        ok = 0;
    }
    if (ok && (CH_CSR & CSR_DONE)) {
        puts_("eDMA: FAIL - phase2: DONE set with 3 of 4 minor loops still to run\r\n");
        ok = 0;
    }
    if (ok && (TCD_CSR & TCD_START)) {
        puts_("eDMA: FAIL - phase2: START not auto-cleared when the channel ran\r\n");
        ok = 0;
    }

    /* Requests #2, #3, #4 -- exactly as the stock edma4/channel_link example
     * issues EDMA_TriggerChannelStart() twice for its CITER=2 channel. */
    TCD_CSR = TCD_START;
    TCD_CSR = TCD_START;
    TCD_CSR = TCD_START;

    /* The 4th START completes the major loop -> DONE. Poll for it (async). */
    if (ok && !wait_done()) {
        puts_("eDMA: FAIL - phase2: major loop complete but DONE not set\r\n");
        ok = 0;
    }
    if (ok && !(dst2[0] == 0xA1A1A1A1u && dst2[1] == 0xB2B2B2B2u &&
                dst2[2] == 0xC3C3C3C3u && dst2[3] == 0xD4D4D4D4u)) {
        puts_("eDMA: FAIL - phase2: 4 STARTs did not copy the buffer byte-exact\r\n");
        ok = 0;
    }
    if (ok && (TCD_CITER & 0x7FFFu) != 4u) {
        puts_("eDMA: FAIL - phase2: CITER not reloaded from BITER at major completion\r\n");
        ok = 0;
    }

    if (ok) {
        puts_("eDMA: PASS - CITER=1 copy + DONE, and CITER=4 needs FOUR STARTs "
              "(one minor loop per service request), byte-exact\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
