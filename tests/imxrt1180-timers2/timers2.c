/*
 * LPTMR1 + GPT1 + TPM1 periodic-interrupt test (Cortex-M33).
 *
 * Each is set up as a periodic timer; verifies every one raises its IRQ
 * repeatedly and its live counter advances.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* LPTMR1 @0x44300000 */
#define LPTMR_CSR (*(volatile uint32_t*)(0x44300000u+0x0))
#define LPTMR_CMR (*(volatile uint32_t*)(0x44300000u+0x8))
#define LPTMR_CNR (*(volatile uint32_t*)(0x44300000u+0xC))
/* GPT1 @0x446C0000 */
#define GPT_CR   (*(volatile uint32_t*)(0x446C0000u+0x0))
#define GPT_SR   (*(volatile uint32_t*)(0x446C0000u+0x8))
#define GPT_IR   (*(volatile uint32_t*)(0x446C0000u+0xC))
#define GPT_OCR1 (*(volatile uint32_t*)(0x446C0000u+0x10))
#define GPT_CNT  (*(volatile uint32_t*)(0x446C0000u+0x24))
/* TPM1 @0x44310000 */
#define TPM_SC  (*(volatile uint32_t*)(0x44310000u+0x10))
#define TPM_CNT (*(volatile uint32_t*)(0x44310000u+0x14))
#define TPM_MOD (*(volatile uint32_t*)(0x44310000u+0x18))

#define ISER(n) (*(volatile uint32_t*)(0xE000E100u+((n)/32)*4))
#define IRQBIT(n) (1u<<((n)%32))
#define LPTMR1_IRQ 18
#define GPT1_IRQ 209
#define TPM1_IRQ 36

static long sh(long op,void*a){register long r0 asm("r0")=op;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void puts_(const char*s){sh(SYS_WRITE0,(void*)s);}
static volatile int lt,gt,tt;
void reset_handler(void); void lptmr_isr(void); void gpt_isr(void); void tpm_isr(void);
__attribute__((section(".vectors"),used))
void(*const vt[])(void)={ [0]=(void(*)(void))STACK_TOP, [1]=reset_handler,
    [16+LPTMR1_IRQ]=lptmr_isr, [16+TPM1_IRQ]=tpm_isr, [16+GPT1_IRQ]=gpt_isr };
void lptmr_isr(void){ LPTMR_CSR |= 0x80; lt++; }        /* W1C TCF */
void gpt_isr(void){ GPT_SR = 0x1; gt++; }               /* W1C OF1 */
void tpm_isr(void){ TPM_SC |= 0x80; tt++; }             /* W1C TOF */

void reset_handler(void)
{
    LPTMR_CMR = 3000; LPTMR_CSR = 0x40|0x1;              /* TIE|TEN */
    GPT_OCR1 = 4000; GPT_IR = 0x1; GPT_CR = 0x1;         /* OF1IE|EN */
    TPM_MOD = 3500; TPM_SC = 0x40|0x08;                  /* TOIE|CMOD=1 */
    ISER(LPTMR1_IRQ)=IRQBIT(LPTMR1_IRQ);
    ISER(GPT1_IRQ)=IRQBIT(GPT1_IRQ);
    ISER(TPM1_IRQ)=IRQBIT(TPM1_IRQ);

    uint32_t l1=LPTMR_CNR, g1=GPT_CNT, t1=TPM_CNT;
    while (lt<2 || gt<2 || tt<2) { }
    int ok = (LPTMR_CNR!=l1) && (GPT_CNT!=g1) && (TPM_CNT!=t1);

    if (ok && lt>=2 && gt>=2 && tt>=2)
        puts_("TIMERS2: PASS - LPTMR + GPT + TPM periodic IRQ + counters run\r\n");
    else
        puts_("TIMERS2: FAIL - a timer did not tick / counter stuck\r\n");
    sh(SYS_EXIT,(void*)0x20026u); for(;;){}
}
