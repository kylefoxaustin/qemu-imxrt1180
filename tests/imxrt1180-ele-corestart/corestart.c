/*
 * S3MU / ELE "kick CM7" core-start command — the reply MCMGR_StartCore blocks on.
 *
 * RT1180's MCMGR_StartCore (mcmgr_internal_core_api_imxrt1180.c) releases the
 * secondary Cortex-M7 through the EdgeLock enclave:
 *
 *     S3MUA->TR[0] = 0x17d20106 ;              // command: KICK CM7 (S401)
 *     while (!(S3MUA->RSR & RF0)) ;            // wait RR[0]
 *     while (!(S3MUA->RSR & RF1)) ;            // wait RR[1]
 *     result1 = S3MUA->RR[0] ;                 // "Should be 0xE1D20206"
 *     result2 = S3MUA->RR[1] ;                 // "Should be 0xD6" (SUCCESS)
 *     ... clear M7_CFG.WAIT -> the M7 runs.
 *
 * The command's OUTCOME -- the CM7 boots -- this model genuinely reproduces (the
 * WAIT-clear starts the already-TCM-zeroed M7), so the enclave answers SUCCESS,
 * not the honest-fault FAILURE used for commands whose result we cannot produce.
 * This test asserts the exact reply words the SDK documents.  If 0xD2 were removed
 * from the S3MU truthful whitelist the enclave would answer RESPONSE_FAILURE and
 * this FAILs -- so it pins the core-start reply, not merely "a reply came back".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

#define S3MU_BASE 0x47540000u
#define MU_TSR (*(volatile uint32_t *)(S3MU_BASE + 0x124))
#define MU_RSR (*(volatile uint32_t *)(S3MU_BASE + 0x12C))
#define MU_TR(i) (*(volatile uint32_t *)(S3MU_BASE + 0x200 + 4 * (i)))
#define MU_RR(i) (*(volatile uint32_t *)(S3MU_BASE + 0x280 + 4 * (i)))

#define KICK_CM7_CMD        0x17d20106u   /* MCMGR_StartCore's TR[0]          */
#define KICK_CM7_RESP_HDR   0xE1D20206u   /* SDK: "Should be 0xE1D20206"      */
#define RESPONSE_SUCCESS    0xD6u         /* SDK: "0xD6"                      */

static long sh(long op, void *a)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = a;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    MU_TR(0) = KICK_CM7_CMD;

    uint32_t spins = 0;
    while ((MU_RSR & 0x3u) != 0x3u) {          /* both reply words present */
        if (++spins >= 20000000u) {
            puts_("ELE-CORESTART: FAIL - enclave never answered the kick-CM7 command\r\n");
            sh(SYS_EXIT, (void *)0x20026u);
        }
    }

    uint32_t r0 = MU_RR(0);
    uint32_t r1 = MU_RR(1);

    if (r0 == KICK_CM7_RESP_HDR && r1 == RESPONSE_SUCCESS) {
        puts_("ELE-CORESTART: PASS - kick-CM7 -> 0xE1D20206 + 0xD6 (SUCCESS)\r\n");
    } else if (r0 == KICK_CM7_RESP_HDR) {
        puts_("ELE-CORESTART: FAIL - correct header but status not SUCCESS "
              "(enclave declined a core-start the model performs)\r\n");
    } else {
        puts_("ELE-CORESTART: FAIL - wrong reply header for the kick-CM7 command\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
