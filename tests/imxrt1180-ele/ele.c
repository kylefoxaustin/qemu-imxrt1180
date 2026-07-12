/*
 * RT1180 EdgeLock (ELE) honesty test — does the enclave LIE TO THE GUEST?
 *
 * Drives the S3MU exactly as the MCUXpresso ELE_RngGetRandom() does:
 *   tmsg = { GET_RNG_RANDOM(0x17CD0407), reseed, &output, size }
 *   -> TR[0..3], then read RR[0..1]
 *   success iff rmsg[0] == GET_RNG_RANDOM_RESPONSE_HDR (0xE1CD0207)
 *              && rmsg[1] == RESPONSE_SUCCESS (0xD6)
 * (constants from ele_crypto_internal.h; check from ele_crypto.c)
 *
 * A model that cannot compute an enclave result MUST NOT report SUCCESS: the
 * guest would take zeroed memory as cryptographic randomness. The enclave is
 * allowed to FAIL (the driver returns kStatus_Fail and firmware handles it);
 * it is not allowed to succeed at nothing.
 */
#include <stdint.h>

#define S3MU_BASE 0x47540000u
#define MU_TSR (*(volatile uint32_t *)(S3MU_BASE + 0x124))
#define MU_RSR (*(volatile uint32_t *)(S3MU_BASE + 0x12C))
#define MU_TR(i) (*(volatile uint32_t *)(S3MU_BASE + 0x200 + 4*(i)))
#define MU_RR(i) (*(volatile uint32_t *)(S3MU_BASE + 0x280 + 4*(i)))

#define GET_RNG_RANDOM              0x17CD0407u
#define GET_RNG_RANDOM_RESPONSE_HDR 0xE1CD0207u
#define RESPONSE_SUCCESS            0xD6u

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
static void sh(int op, void *a){ register int r0 asm("r0")=op; register void*r1 asm("r1")=a;
    asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory"); }
static void puts_(const char *s){ sh(SYS_WRITE0,(void*)s); }
static void hex8(uint32_t v){ char b[11]="0x00000000";
    for(int i=0;i<8;i++){int n=(v>>((7-i)*4))&0xF; b[2+i]=n<10?'0'+n:'a'+n-10;} b[10]=0; puts_(b); }

/* The buffer the enclave is asked to fill with randomness. */
static volatile uint32_t rng_out[8];

extern void reset_handler(void);
__attribute__((section(".vectors"), used))
void (*const vt[])(void) = { [0]=(void(*)(void))0x20020000u, [1]=reset_handler };

void reset_handler(void)
{
    /* Poison the buffer so "untouched" is distinguishable from "wrote zeros". */
    for (int i = 0; i < 8; i++) { rng_out[i] = 0xA5A5A5A5u; }

    /* --- exactly what ELE_RngGetRandom() puts on the wire --- */
    MU_TR(0) = GET_RNG_RANDOM;
    MU_TR(1) = 0u;                      /* reseed flag        */
    MU_TR(2) = (uint32_t)(uintptr_t)rng_out;   /* output buffer */
    MU_TR(3) = sizeof(rng_out);         /* 32 bytes requested */

    while ((MU_RSR & 0x3u) != 0x3u) { } /* wait for 2 reply words */
    uint32_t r0 = MU_RR(0);
    uint32_t r1 = MU_RR(1);

    puts_("ELE reply hdr : "); hex8(r0); puts_("\r\n");
    puts_("ELE reply stat: "); hex8(r1); puts_("\r\n");

    /* the driver's own success test */
    int driver_says_success = (r0 == GET_RNG_RANDOM_RESPONSE_HDR && r1 == RESPONSE_SUCCESS);

    /* did the "enclave" actually produce randomness? */
    int buffer_untouched = 1, all_zero = 1;
    for (int i = 0; i < 8; i++) {
        if (rng_out[i] != 0xA5A5A5A5u) { buffer_untouched = 0; }
        if (rng_out[i] != 0u)          { all_zero = 0; }
    }

    puts_("driver verdict: ");
    puts_(driver_says_success ? "kStatus_Success\r\n" : "kStatus_Fail\r\n");
    puts_("rng buffer    : ");
    puts_(buffer_untouched ? "UNTOUCHED (no randomness produced)\r\n"
                           : (all_zero ? "ALL ZERO\r\n" : "filled\r\n"));

    if (driver_says_success && (buffer_untouched || all_zero)) {
        /* This is the bug: the guest is told crypto-grade success, and gets nothing. */
        puts_("ELE: FAIL - enclave reported SUCCESS but produced NO randomness\r\n");
        puts_("ELE: the guest would seed a crypto stack with un-computed data\r\n");
    } else if (!driver_says_success && (buffer_untouched || all_zero)) {
        /* Honest: cannot compute -> say so, to the GUEST, without hanging it. */
        puts_("ELE: PASS - enclave did not compute, and TOLD THE GUEST (no fake success)\r\n");
    } else {
        puts_("ELE: PASS - enclave returned real data\r\n");
    }
    sh(SYS_EXIT, (void*)0x20026u);
    for (;;) { }
}
