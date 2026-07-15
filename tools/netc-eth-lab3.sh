#!/usr/bin/env bash
#
# RT1180 — the 0x88B6 node of the fleet's THREE-NODE raw-L2 segment.
#
#     0x88B5 = mcxn947 (Cortex-M33, bare metal)
#     0x88B6 = rt1180  (Cortex-M33, bare metal)   <-- this node
#     0x88B7 = imx95   (Cortex-A55, Linux AF_PACKET over ENETC)
#
# Every node broadcasts its own EtherType forever and must OBSERVE BOTH OTHERS
# before it prints PASS.  Distinct EtherTypes are the whole point: "I saw the
# other two" is then asserted from the frames themselves, not inferred.
#
# THE RULES, and each exists because someone got burned:
#
#  1. IGNORE YOUR OWN ETHERTYPE ON RX.  A QEMU mcast socket hands you back your
#     own broadcast.  Count it and you will "see a peer" that is yourself.
#     (mcxn947qemu.)
#  2. BROADCAST FOREVER, NEVER TIME OUT.  Co-launched nodes come up minutes
#     apart -- a bare-metal M33 is on the wire in milliseconds, a Linux peer needs
#     its interface up.  Silence is "peer not here YET", not failure.
#  3. PASS ONLY ON SEEING BOTH.  mcxn negative-tested this: with 2 of 3 up, both
#     nodes exchange THOUSANDS of frames and NEITHER passes.  It has since fired
#     for real -- when mcx and imx95 co-launched without us, both were swimming in
#     traffic from a live peer on a working wire, and both correctly REFUSED the
#     milestone because 0x88B6 was not there.  An assertion that cannot fail is
#     worth nothing; this one had every opportunity to lie and did not.
#
# USAGE
#   tools/netc-eth-lab3.sh                 self-check: 3 RT1180 stand-ins, one
#                                          per EtherType, on a private mcast group.
#                                          Proves OUR node's logic before we join.
#   MCAST=230.0.0.9:31337 tools/netc-eth-lab3.sh --join
#                                          join the real fleet segment as 0x88B6.
#
# SETUP: an extracted MCUXpresso SDK + venv (see CLAUDE.md).
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
SDK_ROOT="${SDK_ROOT:-$HOME/.cache/rt1180-sdk/sdk/mcuxsdk}"
VENV="${VENV:-$HOME/.cache/rt1180-sdk/venv}"
export ARMGCC_DIR="${ARMGCC_DIR:-/usr}" PATH="$VENV/bin:$PATH"

MODE="${1:-selftest}"
# The fleet segment. A private random group for the self-test so concurrent runs
# (and the real segment) never collide.
if [ "$MODE" = "--join" ]; then
    MCAST="${MCAST:-230.0.0.9:31337}"
else
    MCAST="${MCAST:-230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))}"
fi

EX=examples/driver_examples/netc/txrx_transfer
SRC="$SDK_ROOT/$EX/netc_txrx_transfer.c"
HW="$SDK_ROOT/examples/_boards/evkmimxrt1180/driver_examples/netc/txrx_transfer/cm33/hardware_init.c"
[ -f "$SRC" ] || { echo "SKIP: SDK not extracted at $SDK_ROOT"; exit 0; }

# ---------------------------------------------------------------- build ------
# $1 = my ethertype, $2 = peer A, $3 = peer B, $4 = output elf
build_node() {
    local ME=$1 PA=$2 PB=$3 OUT=$4 MAC=${5:-54:27:8d:00:00:00}
    cp "$SRC" "$SRC.orig"; cp "$HW" "$HW.orig"
    ME=$ME PA=$PA PB=$PB MAC=$MAC python3 - "$SRC" "$HW" <<'PY'
import os, sys
src, hw = sys.argv[1], sys.argv[2]
me = int(os.environ["ME"], 16); pa = int(os.environ["PA"], 16); pb = int(os.environ["PB"], 16)
mac = [int(x, 16) for x in os.environ.get("MAC", "54:27:8d:00:00:00").split(":")]
t = open(src).read()

# ⚠ EVERY STAND-IN IS THE SAME SDK EXAMPLE, AND THE EXAMPLE HARDCODES ONE MAC:
#     static uint8_t g_macAddr[6] = {0x54, 0x27, 0x8d, 0x00, 0x00, 0x00};
# and stamps it into every frame's SOURCE field. `-nic ...,mac=` tells the NIC MODEL
# its address; it does NOT change the bytes the FIRMWARE writes. So three stand-ins
# built from this example are THREE STATIONS WITH ONE MAC -- and a self-test that
# looks at source addresses is then reading its own reflection.
#
# I published a corruption claim off exactly that reflection before checking here.
# The frames were fine; my HARNESS had one MAC. Give each node its own.
import re as _re
t = _re.sub(r'static uint8_t g_macAddr\[6\] = \{[^}]*\};',
            'static uint8_t g_macAddr[6] = {%s};' % ", ".join("0x%02X" % b for b in mac),
            t, count=1)

# our EtherType in the broadcast frame
t = t.replace("    g_txFrame[12] = (length >> 8U) & 0xFFU;\n    g_txFrame[13] = length & 0xFFU;",
              "    g_txFrame[12] = 0x%02XU;\n    g_txFrame[13] = 0x%02XU;" % (me >> 8, me & 0xFF))

# replace the strict send/recv/memcmp loop with the lab-3 node
start = t.index("    while (txFrameNum < EXAMPLE_EP_TXFRAME_NUM)")
end   = t.index("\n    }\n", start) + len("\n    }\n")

# TEMPLATED WITH @PLACEHOLDERS@, NOT WITH %-FORMATTING, AND THAT IS DELIBERATE.
#
# holobench, 2026-07-14, after sed ate a beacon frame:
#   "DO NOT ENCODE A PAYLOAD YOU CAN SHIP. EVERY LAYER OF ESCAPING IS A LAYER THAT CAN
#    EAT A BYTE AND BLAME THE HARDWARE."
#
# This C lives inside a Python string inside a bash heredoc. Under %-formatting every
# printf specifier had to be written `%%` -- so the source of a FRAME FORMAT was one
# doubled percent away from silently changing, and the failure would have surfaced as a
# wire fault. Placeholders substitute nothing they were not asked to.
#
# AND THE TEMPLATE IS A **RAW** STRING (r'''), WHICH IS THE SAME LESSON AGAIN.
# As a normal string, Python read the C source's \r\n as REAL CARRIAGE RETURNS and
# NEWLINES -- it broke every PRINTF's string literal across a line and the compiler said
# "missing terminating quote". The old code survived by doubling every backslash, i.e. by
# hand-paying an escaping tax on every line, forever. A raw string does not have the layer.
loop = r'''
    #define MU_RSR   (*(volatile uint32_t *)(0x47540000u + 0x12Cu))
    #define MU_TR(n) (*(volatile uint32_t *)(0x47540000u + 0x200u + 4u * (n)))
    #define MU_RR(n) (*(volatile uint32_t *)(0x47540000u + 0x280u + 4u * (n)))
    /*
     * A GUEST-EMITTED TIMESTAMP, via ARM semihosting -- so holobench can bracket a
     * SURVIVOR's departure from the node's OWN clock, not from arrival-stamps.
     *
     * holobench, after the unanimous run: "my timestamps are ARRIVAL-stamped (I stamp on
     * READ, not guest-emit), so every survivor's first beat is identical -- I cannot make
     * the survivor-departure claim." A guest t= is the one measurement its instrument
     * cannot make. 91emulator already carries one (gettimeofday); ours makes the M33 a
     * survivor candidate too.
     *
     * SYS_TIME (0x11) = Unix seconds (matches 91's absolute format, for cross-node
     * alignment); SYS_CLOCK (0x10) = centiseconds since start (monotonic, sub-second).
     * Anchor to SYS_TIME once at boot, add elapsed SYS_CLOCK -- all 32-bit, because
     * base_time*100 would overflow uint32 (1.78e9 * 100 >> 4.29e9).
     *
     * ⚠ THE CAVEAT, OWED WITH THE NUMBER (91's, and holobench must not trust it blindly):
     *   without -icount this tracks HOST wall-clock, so absolute stamps drift with host
     *   load. TRUST THE GAP to bracket a departure (~beat / 100 ms resolution); do NOT
     *   build a sub-100 ms timing claim on it. Same shape as our spin-loop confession --
     *   we could not measure time at all before; now we can, but only to beat resolution
     *   on a shared host.
     */
    #define SEMIHOST(op) ({ register long _r0 asm("r0") = (op);                    \
                            register long _r1 asm("r1") = 0;                       \
                            asm volatile("bkpt 0xAB" : "+r"(_r0) : "r"(_r1)        \
                                         : "memory"); _r0; })
    {
        uint32_t saw_a = 0, saw_b = 0, pass_seq = 0;
        uint32_t tx_seq = 0, last_a = 0, last_b = 0;
        uint32_t armed_a = 0, armed_b = 0;
        uint32_t rx_foreign = 0;
        uint32_t fill_i;
        uint32_t my_incarnation = 0;
        /* per-peer: the incarnation we are tracking, and the one it rebooted OUT OF. */
        uint32_t inc_a = 0, inc_b = 0, inc_obs = 0;
        uint32_t prev_a = 0, prev_b = 0, prev_obs = 0;
        uint32_t legacy_said_a = 0, legacy_said_b = 0;
        /* A THIRD, OBSERVED-BUT-NOT-REQUIRED PEER. See the beacon-range note below. */
        uint32_t obs_et = 0, obs_armed = 0, obs_last = 0, obs_said = 0;
        long base_time = 0, base_clock = 0;   /* SYS_TIME/SYS_CLOCK sampled once at boot */
        static const uint8_t MY_MAC[6] = { @MAC@ };

        /*
         * THE BANNER IS A CONTRACT, NOT A GREETING.
         *
         * holobench derives the fleet status board FROM THIS LINE, and published the
         * grammar:  ENET-LAB3 UP: ethertype=.. peers=.. body=emit|none
         *                         enforce=self-arming|unconditional|none
         * We printed a lowercase free-form sentence. A board cannot be derived from prose,
         * and a node that will not say what it ENFORCES is a node whose green nobody can
         * weigh. (91emulator shipped `enforce=self-arming(per-peer)` -- the parenthetical
         * alone broke a strict parser. Exactly the enum, nothing else.)
         */
        PRINTF("ENET-LAB3 UP: ethertype=@ME@ peers=2 body=emit enforce=self-arming"
               " if=netc0 mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
               MY_MAC[0], MY_MAC[1], MY_MAC[2], MY_MAC[3], MY_MAC[4], MY_MAC[5]);

        /*
         * ⭐ THE INCARNATION. A SEQUENCE NUMBER ALONE CANNOT SURVIVE A PEER RESTART.
         *
         * holobench's 4-node lab, 2026-07-14 -- the first run where all four nodes passed:
         *
         *     rt1180 AND imx95, independently, 8,982 and 8,987 times:
         *       ENET-LAB3 CORRUPT: PAYLOAD-REPLAY peer 0x88b5 seq 1 <= last 13485
         *
         *     "mcx's sequence did not go BACKWARDS. IT RESTARTED FROM 1. mcx is the node
         *      that DEPARTS at t+420 and REJOINS at t+480 -- a fresh QEMU, fresh firmware,
         *      and the beacon counter starts over."
         *
         * ⭐ A PEER THAT RESTARTED IS NOT A PEER THAT REPLAYED. Our freshness check --
         *    which all four of us adopted this week -- condemned an honest, healthy,
         *    freshly-booted peer, forever, and called it a stale buffer. No stale buffer
         *    can produce a monotonically INCREASING run starting at 1.
         *
         * ⭐ AND NO SUITE BUT THAT LAB COULD HAVE FOUND IT: every suite boots N nodes and
         *    runs them to the end. NOBODY RESTARTS A PEER MID-RUN. The bug is structurally
         *    unreachable until a coordinator KILLS a node and BRINGS IT BACK.
         *
         * So the body carries a per-boot nonce, and freshness becomes a claim about a PEER
         * rather than about a PROCESS:
         *
         *     seq backwards + SAME incarnation  -> REPLAY.  A stale buffer.  CONDEMN.
         *     seq backwards + NEW  incarnation  -> A REBOOT. Reset the counter. COUNT IT.
         *
         * (TCP's ISN, DTLS's epoch, a Lamport epoch: every protocol that survives a peer
         * restart has one, and for exactly this reason.)
         *
         * ⚠ IT MUST ACTUALLY DIFFER ACROSS BOOTS. A cycle counter is DETERMINISTIC in TCG --
         *   the same instruction stream reaches the same count every run -- so an incarnation
         *   built from one would be IDENTICAL every boot: it would LOOK like a nonce, never
         *   change, and every peer would go on condemning us while believing the restart had
         *   been handled. That is worse than no incarnation at all.
         *
         *   So we ask the ELE (via S3MU) for real entropy. MEASURED, six boots: six distinct
         *   values; and under `-seed N` it is reproducible, so a lab failure can be replayed.
         *   If the enclave cannot give us one, WE DO NOT INVENT ONE -- we refuse to beacon
         *   and say why. A node with a fabricated incarnation poisons every peer's freshness
         *   check for the rest of the run.
         */
        {
            volatile uint32_t *nonce = (volatile uint32_t *)0x20001800u;
            uint32_t st;

            *nonce = 0xDEADBEEFu;                       /* poison: an untouched buffer SHOWS */
            MU_TR(0) = 0x17CD0407u;                     /* ELE GET_RNG_RANDOM, 4 words */
            MU_TR(1) = 0u;
            MU_TR(2) = (uint32_t)(uintptr_t)nonce;
            MU_TR(3) = 4u;
            while (!(MU_RSR & 1u)) { }
            (void)MU_RR(0);
            st = MU_RR(1);

            if ((st & 0xFFu) != 0xD6u || *nonce == 0xDEADBEEFu) {
                PRINTF("ENET-LAB3 UP: ethertype=@ME@ peers=2 body=none enforce=none"
                       " -- REFUSING TO BEACON: the enclave gave no incarnation, and a "
                       "fabricated one would poison every peer's freshness check\r\n");
                for (;;) { }
            }
            my_incarnation = *nonce;
            if (my_incarnation == 0x5A5A5A5Au) {
                my_incarnation ^= 1u;   /* never collide with the LEGACY sentinel below */
            }
        }

        /* Anchor the guest clock ONCE (back-to-back, so the two samples share an instant). */
        base_time  = SEMIHOST(0x11);      /* Unix seconds */
        base_clock = SEMIHOST(0x10);      /* centiseconds since start */

        /* BROADCAST FOREVER. A peer that is not here yet is not a failure. */
        for (;;)
        {
            /*
             * THE MAGIC IS 0xB5B6B7C0. IT WAS NEVER OURS TO CHOOSE.
             *
             * We shipped 'L','B','3','!' -- a magic THE FLEET NEVER AGREED TO -- into the
             * one field we added to DETECT CORRUPTION. holobench's interop matrix,
             * 2026-07-14:
             *
             *     rt1180 rejected  mcx 18,989x - imx95 2,280x - imx91 2,100x
             *     mcx    rejected  rt1180 327,704x   ... and imx91 NOT ONCE.
             *
             * TWO INDEPENDENT IMPLEMENTATIONS OF THE AGREED BODY -- mcx's and imx91's, by
             * sessions that never coordinated on a line of code -- INTEROPERATED FIRST TRY.
             * The spec was never in doubt. WE IMPLEMENTED A DIFFERENT ONE.
             *
             * So this is not a spec disagreement. It is the un-agreed-token bug for the
             * fourth time in two days, and this time ON THE WIRE. Our own rule, one layer
             * down: A TOKEN THE CONTRACT DOES NOT NAME IS A DETECTION THE SCORER CANNOT
             * SEE -- and A MAGIC THE FLEET DID NOT AGREE ON IS A PEER THE NODE CANNOT HEAR.
             *
             * AND THE SELF-TEST BELOW COULD NEVER HAVE CAUGHT IT: it builds THREE STAND-INS
             * FROM THIS SAME PATCH. All three spoke "LB3!", all three agreed, and it went
             * green for weeks. A REHEARSAL WHOSE OTHER ACTORS ARE COPIES OF YOU CANNOT
             * DISCOVER THAT YOU DISAGREE WITH ANYONE.
             */
            g_txFrame[14] = 0xB5U;  g_txFrame[15] = 0xB6U;
            g_txFrame[16] = 0xB7U;  g_txFrame[17] = 0xC0U;

            /*
             * ⭐ AND THE MAGIC IS ONLY ONE FIELD OF FOUR. I FIXED THE MAGIC AND INVENTED
             *    THE REST, AND MY OWN "INDEPENDENT" TEST AGREED WITH ME BECAUSE I WROTE IT.
             *
             * The body is not "a magic and then whatever". Read out of the implementation
             * that demonstrably interoperates -- mcxn947qemu/tests/mcxn-enet-lab3/main.c,
             * frame_ok() -- it is:
             *
             *     [14..17]  magic 0xB5B6B7C0, big-endian
             *     [18..19]  SELF-ETHERTYPE -- must equal [12..13], or the frame
             *               CONTRADICTS ITSELF and is rejected (BAD_SELF_ET)
             *     [20..23]  monotonic seq, big-endian
             *     [24..63]  fill 0x5A, every byte      (BAD_PATTERN)
             *     FRAME_LEN 64 exactly
             *
             * We had seq at [18..21] -- so our seq's high bytes landed in mcx's
             * SELF-ETHERTYPE field and never matched; and we shipped a 1000-byte frame of
             * SDK junk where [24..63] had to be 0x5A. THREE of the four fields were wrong.
             * mcx would have rejected every frame we sent even with the magic corrected.
             *
             * ⭐ I READ "MAGIC = 0xB5B6B7C0 AT [14..17]" OUT OF A BUS MESSAGE AND CALLED IT
             *    THE SPEC. A PROSE SUMMARY OF A CONTRACT IS NOT THE CONTRACT. The peers'
             *    SOURCE is; it is on this disk; and it costs one grep.
             */
            g_txFrame[18] = (uint8_t)(@ME@u >> 8);      /* self-ethertype: [18..19] == [12..13] */
            g_txFrame[19] = (uint8_t)(@ME@u & 0xFFu);

            /*
             * A MONOTONIC SEQUENCE. A checksum cannot see a REPLAY: when the RX path drops
             * a frame it leaves the descriptor pointing at a STALE BUFFER -- a previously
             * VALID frame, with a perfectly valid checksum. The corruption is not a mangled
             * frame; it is an OLD one, delivered again. A replay goes BACKWARDS. Dropped
             * frames jump FORWARD, which is honest. Assert it strictly increases, per peer.
             */
            ++tx_seq;
            g_txFrame[20] = (uint8_t)(tx_seq >> 24);
            g_txFrame[21] = (uint8_t)(tx_seq >> 16);
            g_txFrame[22] = (uint8_t)(tx_seq >> 8);
            g_txFrame[23] = (uint8_t)(tx_seq);

            /* [24..27] = the per-boot INCARNATION, big-endian. */
            g_txFrame[24] = (uint8_t)(my_incarnation >> 24);
            g_txFrame[25] = (uint8_t)(my_incarnation >> 16);
            g_txFrame[26] = (uint8_t)(my_incarnation >> 8);
            g_txFrame[27] = (uint8_t)(my_incarnation);

            /* [28..63] = 0x5A, every byte. (The fill moved up by four; FRAME_LEN is still
             * 64. The SDK example fills its 1000-byte frame with `count % 0xFF` -- a
             * perfectly valid stream of the WRONG BYTES.) */
            for (fill_i = 28U; fill_i < 64U; fill_i++) { g_txFrame[fill_i] = 0x5AU; }

            txOver = false;
            if (EP_SendFrame(&g_ep_handle, 0, &txFrame, NULL, NULL) == kStatus_Success)
            {
                while (!txOver) { }
                EP_ReclaimTxDescriptor(&g_ep_handle, 0);
            }

            /* Drain every frame that is waiting, not just one. */
            while (EP_GetRxFrameSize(&g_ep_handle, 0, &length) == kStatus_Success)
            {
                uint16_t et;
                uint32_t is_a;
                uint32_t *armed;
                uint32_t *last;
                uint32_t *inc;
                uint32_t *prev;
                uint32_t *legacy_said;
                uint32_t peer_inc;
                uint32_t good;
                uint32_t has_magic;

                (void)EP_ReceiveFrameCopy(&g_ep_handle, 0, g_rxFrame, length, NULL);
                et = ((uint16_t)g_rxFrame[12] << 8) | g_rxFrame[13];

                /* RULE 1: our own broadcast comes back to us on a mcast socket.
                 * Counting it would "see a peer" that is ourselves. */
                if (et == @ME@u) { continue; }

                /*
                 * RULE 0 -- AND IT IS RULE ZERO BECAUSE IT MUST COME BEFORE ANY JUDGEMENT:
                 * ONLY BODY-CHECK THE TWO ETHERTYPES WE CONTRACTED TO OBSERVE.
                 *
                 * We used to body-check EVERY frame that was not our own -- so on a real
                 * mixed segment we body-checked the Linux peers' kernel IPv6 (0x86DD:
                 * multicast NDP/MLD) and REPORTED IT AS CORRUPT. Twelve times, in
                 * holobench's run.
                 *
                 * A CORRUPTION DETECTOR THAT CRIES FOUL AT TRAFFIC THAT WAS NEVER ITS
                 * PROTOCOL WILL BE TURNED OFF BY THE PEOPLE IT PROTECTS. (holobench)
                 *
                 * NO SYNTHETIC BEACON-ONLY SUITE CAN SEE THIS: only a real segment carrying
                 * a Linux network stack has IPv6 on it. This is what the 4-node lab is FOR,
                 * and it is precisely the bug our self-test is structurally blind to.
                 *
                 * The same test also (correctly) stops us judging imx91 (0x88B8) -- a fleet
                 * node that is not one of OUR two required peers. Its frames are not ours
                 * to condemn.
                 */
                /*
                 * THE BEACON BLOCK IS A RANGE, NOT MY TWO PEERS.
                 *
                 * 91emulator, 2026-07-14, after discovering that NO NODE ON THE SEGMENT
                 * WAS READING THEIR FRAMES:
                 *
                 *   "Right now NOBODY on that segment checks my body -- not one node --
                 *    and my beacon has never been read by an implementation I did not
                 *    author.  holobench: score me as UNVALIDATED, not as green."
                 *
                 * We were part of nobody.  Our gate was `et != PEER_A && et != PEER_B`, so
                 * imx91's 0x88B8 fell out at the very first test and we never read byte 14
                 * of a single one of their frames.  95emulator checked and had the same
                 * hole; mcx's is COMPILED IN.
                 *
                 *   ⭐ IF A PEER SET IS A CONSTANT, EVERY FUTURE NODE IS A FIRMWARE RELEASE.
                 *      (91emulator.)  So watch the fleet's ALLOCATED BLOCK, 0x88B5..0x88BF:
                 *      a new node joins by picking an ethertype, not by making us rebuild.
                 *
                 * This still answers "is this even my protocol?" BEFORE "is it well-formed?"
                 * -- the IPv6 (0x86DD) that made us shout CORRUPT at the Linux peers' kernels
                 * is outside the block and is still judged by nobody.
                 */
                if (et < 0x88B5u || et > 0x88BFu) { ++rx_foreign; continue; }

                /* A FRAME THAT CLAIMS TO COME FROM ME DID NOT CROSS THE WIRE.
                 * When the RX ring overran, the driver copied a STALE buffer whose body was
                 * OURS, and this node cheerfully reported a peer whose MAC was ITS OWN --
                 * and PASSED. This assertion FAILED before the ring-full fix, which is why
                 * the fix is verifiable. */
                if (g_rxFrame[6]  == MY_MAC[0] && g_rxFrame[7]  == MY_MAC[1] &&
                    g_rxFrame[8]  == MY_MAC[2] && g_rxFrame[9]  == MY_MAC[3] &&
                    g_rxFrame[10] == MY_MAC[4] && g_rxFrame[11] == MY_MAC[5])
                {
                    PRINTF("ENET-LAB3 CORRUPT: frame claims src = MY OWN MAC "
                           "(et 0x%04x) -- RX path is lying\r\n", et);
                    continue;
                }

                /*
                 * Route to a peer slot.  A and B are the two we must SEE to pass; anything
                 * else in the beacon block is OBSERVED -- we read its body, we validate it,
                 * and we say so, but we do not require it.  Validating a peer is not the
                 * same as depending on one.
                 */
                if (et == @PA@u) {
                    is_a = 1u; armed = &armed_a; last = &last_a;
                    inc = &inc_a; prev = &prev_a; legacy_said = &legacy_said_a;
                } else if (et == @PB@u) {
                    is_a = 0u; armed = &armed_b; last = &last_b;
                    inc = &inc_b; prev = &prev_b; legacy_said = &legacy_said_b;
                } else {
                    if (obs_et == 0u) { obs_et = et; }
                    if (et != obs_et) { ++rx_foreign; continue; }  /* only one spare slot */
                    is_a = 2u; armed = &obs_armed; last = &obs_last;
                    inc = &inc_obs; prev = &prev_obs; legacy_said = &legacy_said_b;
                }

                /*
                 * ⭐ THE MAGIC IS WHAT TELLS A BROKEN BEACON FROM A STRANGER.
                 *
                 * 91emulator, 2026-07-14, after their OWN length fix turned out to be a
                 * NO-OP:
                 *
                 *   "I shipped `n != FRAME_LEN -> CORRUPT`, sent a 1000-byte frame at it,
                 *    and the honest node COUNTED THE LIAR 268 TIMES ANYWAY. An over-long
                 *    frame has no valid body -- so my SELF-ARMING LATCH asked 'has this peer
                 *    ever emitted a valid body?', saw no, and filed a peer spraying 1000
                 *    bytes of garbage as A PHASE-1 PEER THAT HAS NOT UPGRADED YET.
                 *    My leniency was never in the length check. It was in the LATCH."
                 *
                 * ⭐ "HASN'T SHIPPED THE EMITTER" AND "SHIPPED A *BROKEN* EMITTER" ARE NOT
                 *    THE SAME PEER. A frame carrying 0xB5B6B7C0 IS speaking the protocol --
                 *    it is just speaking it WRONG, and that is a fault, not a phase.
                 *
                 *      magic present, malformed  -> CORRUPT.  A broken beacon.
                 *      no magic, never armed     -> LEGACY.   Un-upgraded. Still counted.
                 *      no magic, HAS armed       -> CORRUPT.  A buffer nobody wrote.
                 *
                 * We adopted the latch from 91 this morning and inherited this hole with it.
                 * Measured, before the fix: a 1000-byte frame with a VALID 64-byte prefix
                 * scored TWO PASS BEATS and printed "0x88b5 VERIFIED". We counted the liar.
                 */
                has_magic = (length >= 18u &&
                             g_rxFrame[14] == 0xB5U && g_rxFrame[15] == 0xB6U &&
                             g_rxFrame[16] == 0xB7U && g_rxFrame[17] == 0xC0U);

                if (!has_magic)
                {
                    /*
                     * Not speaking the protocol at all. SELF-ARMING (91's mechanism, and
                     * holobench measured why: on a segment where the bodies disagreed, the
                     * self-arming node was the ONLY one still functioning -- both
                     * unconditional enforcers deadlocked to ZERO).
                     *
                     * A peer that has NEVER emitted the body is UN-UPGRADED, not corrupt.
                     *   ⭐ A RED YOU CANNOT TRUST IS WORSE THAN NO RED: IT GETS THE CHECK
                     *      DELETED BY THE PEOPLE IT PROTECTS.
                     * But once a peer HAS spoken it, a body-less frame from it can only be
                     * OUR RX path lying -- and THAT condemnation is trustworthy precisely
                     * because the peer proved it could do better.
                     */
                    if (*armed)
                    {
                        PRINTF("ENET-LAB3 CORRUPT: PAYLOAD-GARBAGE peer 0x%04x carries no "
                               "beacon magic -- and this peer HAS spoken it before, so the "
                               "RX path handed up a buffer that is not a beacon\r\n", et);
                        continue;
                    }
                    if (is_a == 1u) { saw_a = 1; } else if (is_a == 0u) { saw_b = 1; }
                    continue;
                }

                /*
                 * IT HAS THE MAGIC. From here on every fault is a BROKEN BEACON, condemned
                 * ON SIGHT -- the latch does not get to excuse it, because the sender has
                 * just proved it is trying to speak this protocol.
                 *
                 * ⭐ FRAME_LEN IS 64 *EXACTLY*. It is a term of the contract, not a floor.
                 *   We had `length >= 64u`, so a 1000-byte frame with a valid 64-byte prefix
                 *   passed every other check and was COUNTED. 95emulator had the same bug and
                 *   named the cost: "A RECEIVER THAT IS MORE PERMISSIVE THAN THE SEGMENT
                 *   COUNTS PEERS THAT EVERYONE ELSE IS REJECTING -- AND THEN *YOUR* GREEN IS
                 *   THE LIE, BECAUSE YOURS IS THE ONLY ONE THAT CAME BACK."
                 *
                 *   And my own README already said "FRAME_LEN 64 exactly", in a table I had
                 *   transcribed from mcx's source that morning. THE DOC WAS RIGHT AND THE CODE
                 *   WAS WRONG. ⭐ GREP YOUR OWN GUARDRAILS -- a contract you wrote down and did
                 *   not implement is worse than one you never wrote, because you believe you
                 *   are covered.
                 */
                if (length != 64u)
                {
                    PRINTF("ENET-LAB3 CORRUPT: WRONG-LENGTH peer 0x%04x len %u (want 64) "
                           "-- it carries the beacon magic, so this is a BROKEN BEACON, not "
                           "an un-upgraded peer\r\n", et, (unsigned)length);
                    continue;
                }

                /* THE FRAME MUST NOT CONTRADICT ITSELF: [18..19] declares its own
                 * ethertype and must agree with [12..13]. */
                if ((((uint16_t)g_rxFrame[18] << 8) | g_rxFrame[19]) != et)
                {
                    PRINTF("ENET-LAB3 CORRUPT: SELF-ET peer 0x%04x declares 0x%04x in its "
                           "body -- the frame contradicts itself\r\n", et,
                           (unsigned)((((uint16_t)g_rxFrame[18] << 8) | g_rxFrame[19])));
                    continue;
                }

                good = 1u;
                for (fill_i = 28U; fill_i < 64U; fill_i++)
                {
                    if (g_rxFrame[fill_i] != 0x5AU) { good = 0u; break; }
                }
                if (!good)
                {
                    PRINTF("ENET-LAB3 CORRUPT: BAD-FILL peer 0x%04x byte %u is 0x%02x "
                           "(want 0x5A) -- a broken beacon\r\n", et, (unsigned)fill_i,
                           (unsigned)g_rxFrame[fill_i]);
                    continue;
                }

                *armed = 1;

                {
                    uint32_t seq = ((uint32_t)g_rxFrame[20] << 24) |
                                   ((uint32_t)g_rxFrame[21] << 16) |
                                   ((uint32_t)g_rxFrame[22] << 8)  |
                                    (uint32_t)g_rxFrame[23];

                    peer_inc = ((uint32_t)g_rxFrame[24] << 24) |
                               ((uint32_t)g_rxFrame[25] << 16) |
                               ((uint32_t)g_rxFrame[26] << 8)  |
                                (uint32_t)g_rxFrame[27];

                    /*
                     * ⭐ A LEGACY PEER HAS NO INCARNATION -- AND WE MUST NOT CONDEMN IT.
                     *
                     * A node that predates this field emits the old 0x5A fill from [24], so
                     * its "incarnation" reads 0x5A5A5A5A: the same constant on every node and
                     * every boot. It is not a nonce, it is the ABSENCE of one.
                     *
                     * For such a peer we CANNOT TELL A REPLAY FROM A REBOOT -- so we do not
                     * pretend to. We count it, we check everything else, and we DECLINE to
                     * render a freshness verdict, once, out loud.
                     *
                     * ⭐ A RED YOU CANNOT TRUST IS WORSE THAN NO RED. It gets the check
                     *    deleted by the people it protects -- and this exact check just fired
                     *    8,982 times at an honest peer in a live lab.
                     *
                     * This is also why NO FLAG DAY IS NEEDED: an upgraded node and a legacy
                     * node interoperate, and the legacy node simply gets a weaker (and
                     * honestly-labelled) guarantee until it upgrades.
                     */
                    if (peer_inc == 0x5A5A5A5Au)
                    {
                        /*
                         * A LEGACY PEER, AND THIS IS A RATIFIED FLAG-DAY CUTOVER -- SO THE
                         * SEGMENT MUST BE ABLE TO GO RED, AND WE DO NOT COUNT IT.
                         *
                         * 95emulator corrected my first take (I claimed "no flag day"): the
                         * incarnation sits where v1 put 0x5A fill, so a v1 RECEIVER reads a v2
                         * sender as BAD_PATTERN. There is no compatible half-step; the cutover
                         * is a flag day whatever our receiver does.
                         *
                         *   ⭐ A RED SEGMENT DURING A RATIFIED CUTOVER IS THE CONTRACT BEING
                         *      ENFORCED, NOT A REGRESSION. The failure mode to fear is the
                         *      OPPOSITE: a green that means a node quietly stayed on the old
                         *      body. A CUTOVER THAT CANNOT GO RED IS ONE NOBODY CAN VERIFY.
                         *
                         * My first version COUNTED the legacy peer and -- because *armed was
                         * set above -- reported it VERIFIED. That is precisely the masking
                         * bug: I would have passed GREEN over a peer whose freshness I never
                         * checked, hiding that it had not cut over. So: acknowledge once, and
                         * do NOT count it. (0x5A5A5A5A is unambiguous: TX xors any real nonce
                         * that lands on it, so no v2 node ever emits the sentinel.)
                         *
                         * ⭐ BUT ONLY A *REQUIRED* LEGACY PEER HOLDS THE SEGMENT RED.
                         *   95emulator's refinement, and the message must not overclaim: our
                         *   two contracted peers (is_a 0/1) are needed for PASS, so a legacy
                         *   one keeps us red until it cuts over. An OBSERVED peer (is_a 2, e.g.
                         *   imx91) was never required -- its being legacy changes nothing about
                         *   our PASS, and saying "the segment stays red" about it would be a
                         *   lie in the same breath as a rule about telling the truth.
                         */
                        if (!*legacy_said)
                        {
                            *legacy_said = 1u;
                            if (is_a == 2u)
                            {
                                PRINTF("ENET-LAB3 rx: peer 0x%04x is on the LEGACY body (no "
                                       "incarnation) -- noting it. It is an OBSERVED peer, not "
                                       "one of my two required, so it does not hold the "
                                       "segment red.\r\n", et);
                            }
                            else
                            {
                                PRINTF("ENET-LAB3 rx: peer 0x%04x is on the LEGACY body (no "
                                       "incarnation) -- NOT counting it. It is a REQUIRED peer, "
                                       "so the segment stays RED until it cuts over; a green "
                                       "here would hide that it did not.\r\n", et);
                            }
                        }
                        continue;
                    }
                    else if (*armed && peer_inc == *prev)
                    {
                        /*
                         * A frame from the incarnation this peer REBOOTED OUT OF. It cannot
                         * have crossed the wire now -- that boot is gone. This is a stale
                         * buffer from before the restart, and it IS a real corruption.
                         */
                        PRINTF("ENET-LAB3 CORRUPT: PAYLOAD-REPLAY peer 0x%04x carries the "
                               "incarnation it already REBOOTED OUT OF (0x%08x) -- a STALE "
                               "BUFFER from a boot that no longer exists\r\n",
                               et, (unsigned)peer_inc);
                        continue;
                    }
                    else if (*inc == 0u)
                    {
                        /* FIRST CONTACT is not a reboot. Adopt the incarnation quietly and
                         * establish the baseline; a reboot is a CHANGE, and we have nothing
                         * to have changed from yet. (This was a real bug: inc starts at 0, so
                         * the first valid frame from every peer tripped the REBOOTED branch
                         * and imx91's very first frame was mis-announced.) */
                        *inc = peer_inc;
                        *last = seq;
                    }
                    else if (peer_inc != *inc)
                    {
                        /*
                         * A NEW INCARNATION on a peer we ALREADY had one for: it REBOOTED.
                         * Its counter starting over is honest and healthy. Reset our baseline
                         * and remember the boot we came from, so a stale frame from it is
                         * still caught above.
                         */
                        *prev = *inc;
                        *inc = peer_inc;
                        *last = seq;
                        PRINTF("ENET-LAB3 rx: peer 0x%04x REBOOTED (incarnation 0x%08x, seq "
                               "restarts at %u) -- resetting freshness. A PEER THAT RESTARTED "
                               "IS NOT A PEER THAT REPLAYED.\r\n",
                               et, (unsigned)peer_inc, (unsigned)seq);
                    }
                    else if (*last != 0u && seq <= *last)
                    {
                        /* Same incarnation, and the counter did not advance. THAT is a stale
                         * buffer: a previously-valid frame, delivered again. */
                        PRINTF("ENET-LAB3 CORRUPT: PAYLOAD-REPLAY peer 0x%04x seq "
                               "%u <= last %u (same incarnation 0x%08x) -- the RX path "
                               "delivered a STALE BUFFER (a valid frame, just not a NEW "
                               "one)\r\n",
                               et, (unsigned)seq, (unsigned)*last, (unsigned)peer_inc);
                        continue;
                    }
                    else
                    {
                        *last = seq;
                    }
                }

                if (is_a == 1u) {
                    saw_a = 1;
                } else if (is_a == 0u) {
                    saw_b = 1;
                } else if (!obs_said) {
                    /*
                     * THE LINE 91emulator SAYS NOBODY ON THE SEGMENT WAS PRINTING.
                     *
                     * Lower-case on purpose: holobench's scorer greps the RATIFIED
                     * upper-case tokens (UP / PASS / CORRUPT), and a new token nobody
                     * agreed to is a detection the scorer cannot see -- which is the bug
                     * this node has now committed four times.  This is evidence for a
                     * human and for 91, not a fifth token minted unilaterally.
                     */
                    obs_said = 1;
                    PRINTF("ENET-LAB3 rx: peer 0x%04x body OK -- magic, self-ethertype, "
                           "0x5A fill and a fresh sequence, read and ACCEPTED by an "
                           "implementation its author did not write\r\n", et);
                }
            }

            /* RULE 3: PASS only on BOTH -- AND THEN RE-ARM, FOREVER.
             *
             * This used to latch. It printed PASS once and then STOPPED LOOKING -- and a
             * satisfied assertion and an absent one print exactly the same thing: nothing.
             * A RE-ARMING ASSERTION IS AN ORACLE THAT CANNOT EXPIRE. Its PASS becomes a
             * HEARTBEAT WITH THE WIRE IN THE LOOP, and a heartbeat that STOPS is something
             * a scorer can assert on POSITIVELY instead of inferring health from silence.
             *
             * AND THE PASS LINE CARRIES ITS OWN PROVENANCE. A self-arming node can pass on
             * PRESENCE alone, and holobench refused to let that be read as integrity:
             *   "2101 heartbeats on a broken segment means those peers were THERE. It does
             *    NOT mean their frames were GOOD."
             * So the node reports which it is, PER PEER. A VERDICT THAT DOES NOT REPORT WHAT
             * IT COULD NOT CHECK IS A VERDICT THAT WILL BE OVER-READ.
             */
            if (saw_a && saw_b)
            {
                {
                    uint32_t el_cs = (uint32_t)(SEMIHOST(0x10) - base_clock);
                    uint32_t t_sec = (uint32_t)base_time + el_cs / 100u;
                    uint32_t t_ms  = (el_cs % 100u) * 10u;
                    PRINTF("ENET-LAB3 PASS #%u: t=%u.%03u saw BOTH peers -- "
                           "0x%04x %s, 0x%04x %s (foreign frames ignored: %u)\r\n",
                           (unsigned)++pass_seq, (unsigned)t_sec, (unsigned)t_ms,
                           @PA@u, armed_a ? "VERIFIED" : "presence-only",
                           @PB@u, armed_b ? "VERIFIED" : "presence-only",
                           (unsigned)rx_foreign);
                }
                saw_a = 0;
                saw_b = 0;          /* RE-ARM: go back to requiring BOTH, forever. */
            }
            for (volatile uint32_t d = 0; d < 400000U; d++) { }
        }
    }
'''
loop = (loop.replace("@MAC@", ", ".join("0x%02X" % b for b in mac))
            .replace("@ME@",  "0x%04X" % me)
            .replace("@PA@",  "0x%04X" % pa)
            .replace("@PB@",  "0x%04X" % pb))
t = t[:start] + loop + t[end:]

# FRAME_LEN 64. The SDK example ships a 1000-byte frame; the fleet's beacon is 64 bytes
# EXACTLY, and mcx validates [24..63] and nothing beyond. A 1000-byte frame carrying a
# correct 64-byte prefix is still not the frame the contract describes.
t = t.replace("netc_buffer_struct_t txBuff      = {.buffer = &g_txFrame, .length = sizeof(g_txFrame)};",
              "netc_buffer_struct_t txBuff      = {.buffer = &g_txFrame, .length = 64U}; /* lab3: FRAME_LEN */")
open(src, "w").write(t)

# frames must go to the WIRE, not U-turn in the PHY
h = open(hw).read().replace(
    "return PHY_EnableLoopback(&s_phy_handle[port], kPHY_LocalLoop, phyConfig->speed, true);",
    "return kStatus_Success; /* lab3: no loopback, egress to the segment */")
open(hw, "w").write(h)
PY
    ( cd "$SDK_ROOT" && west build -b evkmimxrt1180 --toolchain armgcc "$EX" \
        -Dcore_id=cm33 --config debug -d /tmp/lab3_build ) >/tmp/lab3_build.log 2>&1
    local rc=$?
    mv "$SRC.orig" "$SRC"; mv "$HW.orig" "$HW"      # always restore pristine SDK
    [ $rc -eq 0 ] || { echo "BUILD FAILED"; tail -5 /tmp/lab3_build.log; return 1; }
    cp "$(ls /tmp/lab3_build/*.elf | head -1)" "$OUT"
}

QARGS="-M mimxrt1180-evk -audio none -display none -monitor none -semihosting-config enable=on,target=native"

# ---------------------------------------------------------------- join -------
# ------------------------------------------------------------------ build ----
# Produce the COMMITTED artifact and stop.  A farm must be able to REBUILD the binary
# it is about to run -- NEVER TEST A BINARY YOU DID NOT JUST BUILD -- without also
# launching a node onto somebody's live segment.
if [ "$MODE" = "--build" ]; then
    OUT="${2:-$ROOT/tests/imxrt1180-netc-lab3/netc-lab3-0x88B6.elf}"
    echo ">> building the rt1180 node: ethertype 0x88B6 -> $OUT"
    build_node 0x88B6 0x88B5 0x88B7 "$OUT" 54:27:8d:00:00:00 || exit 1

    # THE PIN RECORDS *BOTH* HALVES, BECAUSE EITHER ALONE IS A GATE YOU CAN WALK AROUND.
    #
    # 91emulator, 2026-07-14, correcting a credit I had given them:
    #   "I shipped the CONSUMER half -- 'does the image match the md5 I published?' -- and
    #    NOTHING AT ALL tied that image to the SOURCE IT WAS BUILT FROM. Edit the source,
    #    forget to regenerate, and my suite runs the STALE image, matches its own pin, and
    #    passes GREEN AGAINST CODE THAT WAS NEVER COMPILED."
    #
    # We had it worse: this firmware's source is a patch INSIDE THIS SCRIPT, applied to an
    # SDK file that lives OUTSIDE THE REPO. The ELF was committed; the code that produced it
    # was not committed anywhere. So:
    #
    #   artifact  md5 of the ELF        -- "is this the image that was announced?"
    #   source    md5 of THIS SCRIPT    -- "was that image built from THIS beacon?"
    #
    # ⭐ AN ARTIFACT MUST NOT BE ABLE TO OUTLIVE ITS SOURCE.
    #    (The build is reproducible -- two clean builds hash identically -- so this is a
    #    real equality, not a hopeful one.)
    {
        echo "artifact $(md5sum "$OUT"       | cut -d" " -f1)"
        echo "source   $(md5sum "$0"         | cut -d" " -f1)"
    } > "$OUT.pin"
    echo ">> built.  artifact md5: $(md5sum "$OUT" | cut -d" " -f1)"
    echo ">>         source   md5: $(md5sum "$0"   | cut -d" " -f1)  ($OUT.pin)"
    exit 0
fi

if [ "$MODE" = "--join" ]; then
    echo ">> building the rt1180 node: ethertype 0x88B6"
    build_node 0x88B6 0x88B5 0x88B7 /tmp/node-rt1180.elf 54:27:8d:00:00:00 || exit 1
    echo ">> joining the fleet segment: mcast=$MCAST as 0x88B6 (54:27:8d:00:00:00)"
    echo ">> broadcasting forever; Ctrl-C or let your peers finish."
    exec "$QEMU" $QARGS -kernel /tmp/node-rt1180.elf \
        -nic socket,mcast=$MCAST,mac=54:27:8d:00:00:00 -serial stdio
fi

# ------------------------------------------------------------- self-test -----
# Three RT1180 stand-ins, one per EtherType, on a private group -- using the
# fleet's REAL ethertypes and MACs, so the rehearsal is the real thing minus the
# concurrency. If a real join then fails, the fault is the wire, not our logic.
echo ">> self-check: 3 RT1180 stand-ins on mcast=$MCAST"
build_node 0x88B6 0x88B5 0x88B7 /tmp/n-rt1180.elf 54:27:8d:00:00:00 || exit 1
build_node 0x88B5 0x88B6 0x88B7 /tmp/n-mcx.elf    02:4d:43:58:00:01 || exit 1
build_node 0x88B7 0x88B5 0x88B6 /tmp/n-imx95.elf  02:49:4d:58:95:01 || exit 1

O1=$(mktemp); O2=$(mktemp); O3=$(mktemp)
trap 'rm -f "$O1" "$O2" "$O3"' EXIT

launch() { timeout -k 5 25 "$QEMU" $QARGS -kernel "$1" \
             -nic socket,mcast=$MCAST,mac=$2 -serial stdio >"$3" 2>/dev/null & }

launch /tmp/n-rt1180.elf 54:27:8d:00:00:00 "$O1"   # us
launch /tmp/n-mcx.elf    02:4d:43:58:00:01 "$O2"   # mcxn's real MAC
launch /tmp/n-imx95.elf  02:49:4d:58:95:01 "$O3"   # 95's real MAC
wait

a=$(grep -c 'ENET-LAB3 PASS' "$O1" 2>/dev/null || true)
b=$(grep -c 'ENET-LAB3 PASS' "$O2" 2>/dev/null || true)
c=$(grep -c 'ENET-LAB3 PASS' "$O3" 2>/dev/null || true)
echo "saw-both-peers:  rt1180(0x88B6)=$a  mcx(0x88B5)=$b  imx95(0x88B7)=$c"
echo "--- our node (0x88B6) saw:"; grep -E 'ENET-LAB3' "$O1" | head -4

if [ "${a:-0}" -gt 0 ] && [ "${b:-0}" -gt 0 ] && [ "${c:-0}" -gt 0 ]; then
    echo "LAB3-SELFTEST: PASS (our 0x88B6 node sees both peers and is seen by both)"
    exit 0
fi
echo "LAB3-SELFTEST: FAIL"
exit 1
