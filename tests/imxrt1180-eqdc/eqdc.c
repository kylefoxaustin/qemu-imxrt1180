/*
 * EQDC quadrature-encoder test (Cortex-M33).
 *
 * Verifies the EQDC model behaviours a FOC encoder driver relies on:
 *   1. CTRL.LDOK is self-clearing (firmware sets it, reads it back cleared).
 *   2. CTRL.SWIP + LDOK preloads the position counter from UINIT:LINIT.
 *   3. Coherent 32-bit read: reading UPOS snapshots LPOS/REV/POSD into their
 *      hold registers, so (UPOS, LPOSH) and REVH/POSDH are consistent.
 *   4. No plant: the position does not advance on its own.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* EQDC1 @ 0x4271_0000 (16-bit registers). */
#define EQDC1_BASE 0x42710000u
#define CTRL   (*(volatile uint16_t *)(EQDC1_BASE + 0x00))
#define UPOS   (*(volatile uint16_t *)(EQDC1_BASE + 0x0C))
#define LPOS   (*(volatile uint16_t *)(EQDC1_BASE + 0x0E))
#define POSD   (*(volatile uint16_t *)(EQDC1_BASE + 0x10))
#define POSDH  (*(volatile uint16_t *)(EQDC1_BASE + 0x12))
#define LPOSH  (*(volatile uint16_t *)(EQDC1_BASE + 0x16))
#define REVH   (*(volatile uint16_t *)(EQDC1_BASE + 0x1C))
#define REV    (*(volatile uint16_t *)(EQDC1_BASE + 0x1E))
#define UINIT  (*(volatile uint16_t *)(EQDC1_BASE + 0x20))
#define LINIT  (*(volatile uint16_t *)(EQDC1_BASE + 0x22))

#define CTRL_LDOK 0x0001u
#define CTRL_SWIP 0x0800u

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
void (* const vt[])(void) = {
    [0] = (void (*)(void))STACK_TOP,
    [1] = reset_handler,
};

void reset_handler(void)
{
    int ok = 1;

    /* (2) Software-init preload of position from UINIT:LINIT. */
    UINIT = 0x1234;
    LINIT = 0x5678;
    CTRL  = CTRL_SWIP | CTRL_LDOK;

    /* (1) LDOK self-cleared, SWIP retained. */
    if (CTRL != CTRL_SWIP) { ok = 0; }

    /* (3) Coherent read: UPOS read snapshots LPOS into LPOSH. */
    if (UPOS != 0x1234) { ok = 0; }       /* also triggers the snapshot */
    if (LPOSH != 0x5678) { ok = 0; }      /* lower half from the hold reg */

    /* REV / POSD snapshot on the next UPOS read. */
    REV  = 0xAAAA;
    POSD = 0xBBBB;
    (void)UPOS;                           /* snapshot REV/POSD -> hold */
    if (REVH != 0xAAAA) { ok = 0; }
    if (POSDH != 0xBBBB) { ok = 0; }

    /* (4) No plant: position is unchanged after a delay. */
    uint16_t p1 = UPOS;
    for (volatile int i = 0; i < 500; i++) {
    }
    if (UPOS != p1) { ok = 0; }

    if (ok) {
        puts_("EQDC: PASS - LDOK self-clear + SWIP preload + coherent hold read\r\n");
    } else {
        puts_("EQDC: FAIL - LDOK / SWIP / snapshot misbehaved\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
