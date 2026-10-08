/*
 * i.MX RT1180 — SAI RX deterministic-source value test (Cortex-M33).
 *
 * The SAI RX path has no host input device; the capture stream is a pure function
 * of the sample index, a spec SHARED with the rt1180renode Renode model so both
 * emulators clock the SAME stream into RX:  sample(n) = (uint16_t)(n*2654435761u>>16),
 * n reset to 0 on RCSR.RE 0->1, ++ per word.  This test enables RX, reads RDR0 as
 * the FRF flag reports data, and checks the popped words are exactly sample(0),
 * sample(1), ... -- a FIRST-PRINCIPLES golden (the hash is recomputed here, not
 * taken from the model), plus the equivalency gate: NOT all-zero and NOT constant
 * (a silent/stalled model and a constant source both produce a degenerate stream).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

#define SAI1      0x443B0000u
#define RCSR (*(volatile uint32_t *)(SAI1 + 0x88))
#define RCR1 (*(volatile uint32_t *)(SAI1 + 0x8C))
#define RDR0 (*(volatile uint32_t *)(SAI1 + 0xA0))
#define RFR0 (*(volatile uint32_t *)(SAI1 + 0xC0))
#define CSR_RE  (1u << 31)   /* receiver enable */
#define CSR_FRF (1u << 16)   /* RX FIFO request: data at/above watermark */

#define N 16

static long sh(long o, void *a){register long r0 asm("r0")=o;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void puts_(const char *s){sh(SYS_WRITE0,(void*)s);}
static void px(uint32_t i,uint32_t v){static const char h[]="0123456789abcdef";char b[16]={'s',h[(i>>4)&15],h[i&15],'=','0','x',h[(v>>12)&15],h[(v>>8)&15],h[(v>>4)&15],h[v&15],'\r','\n',0};sh(SYS_WRITE0,(void*)b);}

static uint16_t gen(uint32_t n){ return (uint16_t)((n * 2654435761u) >> 16); }

void reset_handler(void);
__attribute__((section(".vectors"),used)) void(*const vt[])(void)={[0]=(void(*)(void))STACK_TOP,[1]=reset_handler};

void reset_handler(void)
{
    uint16_t got[N];

    RCR1 = 0;                 /* RX watermark 0 -> FRF when count > 0 */
    RCSR = CSR_RE;            /* enable RX: resets the generator, arms the fs fill */

    for (int i = 0; i < N; i++) {
        while (!(RCSR & CSR_FRF)) { }     /* wait for a captured word */
        got[i] = (uint16_t)(RDR0 & 0xFFFFu);
    }

    int seq_ok = 1, nonzero = 0, nonconst = 0;
    for (int i = 0; i < N; i++) {
        if (got[i] != gen(i)) seq_ok = 0;
        if (got[i] != 0) nonzero = 1;
        if (got[i] != got[0]) nonconst = 1;
    }
    for (int i = 0; i < 8; i++) px(i, got[i]);

    if (seq_ok && nonzero && nonconst) {
        puts_("SAI-RX: PASS - captured stream == sample(n) generator, non-zero, non-constant\r\n");
    } else if (!seq_ok) {
        puts_("SAI-RX: FAIL - captured stream does not match the sample(n) golden\r\n");
    } else {
        puts_("SAI-RX: FAIL - degenerate stream (all-zero or constant)\r\n");
    }
    sh(SYS_EXIT,(void*)0x20026u);
    for(;;){}
}
