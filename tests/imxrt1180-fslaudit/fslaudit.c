#include <stdint.h>
#define SYS_WRITE0 0x04
#define SYS_EXIT 0x18
static long sh(long o,void*a){register long r0 asm("r0")=o;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void p(const char*s){sh(SYS_WRITE0,(void*)s);}
void r(void);
__attribute__((section(".vectors"),used)) void(*const vt[])(void)={[0]=(void(*)(void))0x20020000u,[1]=r};
void r(void){
  volatile uint32_t*gpt=(volatile uint32_t*)0x446C0000u;   /* GPT1 CR@0 */
  gpt[0] |= 0x8000;                    /* CR.SWR (GPT_SoftwareReset) */
  uint32_t spins=0;
  while (gpt[0] & 0x8000) { if(++spins>1000000){ p("GPT_INIT: FAIL - SWR stuck (would hang fsl)\r\n"); sh(SYS_EXIT,(void*)0x20026u);} }
  /* TPM: write a channel value + read it back (TPM_SetupPwm poll) */
  volatile uint32_t*tpm=(volatile uint32_t*)0x44310000u;
  tpm[0x24/4]=1234;                    /* CONTROLS[0].CnV */
  if (tpm[0x24/4]!=1234){ p("TPM_PWM: FAIL - CnV read-back mismatch (would hang fsl)\r\n"); sh(SYS_EXIT,(void*)0x20026u); }
  p("FSLAUDIT: PASS - GPT SWR self-clears + TPM CnV read-back matches\r\n");
  sh(SYS_EXIT,(void*)0x20026u); for(;;){}
}
