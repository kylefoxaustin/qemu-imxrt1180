/*
 * SEMA42 + VREF + CMP smoke test (Cortex-M33).
 *  - SEMA42 gate lock/unlock mutual-exclusion semantics.
 *  - VREF reports stable once enabled.
 *  - CMP config sticks + CFR/CFF are W1C.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u
#define SEMA1_G0 (*(volatile uint8_t  *)(0x44260000u + 0))
#define VREF_CSR (*(volatile uint32_t *)(0x42E30000u + 0x8))
#define CMP1_C0  (*(volatile uint32_t *)(0x42DC0000u + 0x8))
static long sh(long op,void*a){register long r0 asm("r0")=op;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void puts_(const char*s){sh(SYS_WRITE0,(void*)s);}
void reset_handler(void);
__attribute__((section(".vectors"),used)) void(*const vt[])(void)={[0]=(void(*)(void))STACK_TOP,[1]=reset_handler};
void reset_handler(void)
{
    int ok = 1;
    /* SEMA42: lock gate to domain 1, a second locker fails, owner unlocks. */
    SEMA1_G0 = 1;        if (SEMA1_G0 != 1) ok = 0;    /* locked to 1 */
    SEMA1_G0 = 2;        if (SEMA1_G0 != 1) ok = 0;    /* still owned by 1 */
    SEMA1_G0 = 0;        if (SEMA1_G0 != 0) ok = 0;    /* unlocked */
    /* VREF: enable -> stable bit reads set. */
    VREF_CSR = 0x1;      if (!(VREF_CSR & 0x4)) ok = 0;
    /* CMP: config sticks; CFR (bit26) is W1C. */
    CMP1_C0 = 0x00000021;   if ((CMP1_C0 & 0xFF) != 0x21) ok = 0;
    CMP1_C0 = 0x04000000;   /* try to set CFR -> read-only-ish, cleared */
    if (CMP1_C0 & 0x04000000) ok = 0;

    puts_(ok ? "MISC1: PASS - SEMA42 gate + VREF stable + CMP regs\r\n"
             : "MISC1: FAIL\r\n");
    sh(SYS_EXIT,(void*)0x20026u); for(;;){}
}
