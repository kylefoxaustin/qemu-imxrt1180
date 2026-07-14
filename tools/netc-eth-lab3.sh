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
    {
        uint32_t saw_a = 0, saw_b = 0, pass_seq = 0;
        uint32_t tx_seq = 0, last_a = 0, last_b = 0;
        uint32_t armed_a = 0, armed_b = 0;
        uint32_t rx_foreign = 0;
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
             * A MONOTONIC SEQUENCE. A checksum cannot see a REPLAY: when the RX path drops
             * a frame it leaves the descriptor pointing at a STALE BUFFER -- a previously
             * VALID frame, with a perfectly valid checksum. The corruption is not a mangled
             * frame; it is an OLD one, delivered again. A replay goes BACKWARDS. Dropped
             * frames jump FORWARD, which is honest. Assert it strictly increases, per peer.
             */
            ++tx_seq;
            g_txFrame[18] = (uint8_t)(tx_seq >> 24);
            g_txFrame[19] = (uint8_t)(tx_seq >> 16);
            g_txFrame[20] = (uint8_t)(tx_seq >> 8);
            g_txFrame[21] = (uint8_t)(tx_seq);

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
                uint32_t good;

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
                if (et != @PA@u && et != @PB@u) { ++rx_foreign; continue; }

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

                is_a  = (et == @PA@u);
                armed = is_a ? &armed_a : &armed_b;
                last  = is_a ? &last_a  : &last_b;

                good = (g_rxFrame[14] == 0xB5U && g_rxFrame[15] == 0xB6U &&
                        g_rxFrame[16] == 0xB7U && g_rxFrame[17] == 0xC0U);

                if (!good)
                {
                    /*
                     * SELF-ARMING, PER PEER. (91emulator's mechanism; holobench MEASURED it:
                     * on a segment where the body formats disagreed, the self-arming node was
                     * THE ONLY ONE THAT STILL FUNCTIONED. Both UNCONDITIONAL enforcers --
                     * mcx and us -- deadlocked to ZERO heartbeats.)
                     *
                     * A peer that has NEVER emitted the agreed body is an UN-UPGRADED PEER,
                     * not a corrupt frame. Condemning it is a claim we have not earned, and
                     *   A RED YOU CANNOT TRUST IS WORSE THAN NO RED: IT GETS THE CHECK
                     *   DELETED BY THE PEOPLE IT PROTECTS.
                     * So degrade to PRESENCE -- and SAY SO, in the PASS line.
                     *
                     * But once a peer HAS spoken the body it is ARMED, and garbage from it
                     * can only be OUR RX path lying. THAT we condemn -- and that CORRUPT is
                     * trustworthy precisely because the peer proved it could do better.
                     * The assertion earns the right to fire.
                     */
                    if (*armed)
                    {
                        PRINTF("ENET-LAB3 CORRUPT: PAYLOAD-GARBAGE peer 0x%04x carries no "
                               "beacon magic -- and this peer HAS spoken it before, so the "
                               "RX path handed up a buffer that is not a beacon\r\n", et);
                        continue;
                    }
                    if (is_a) { saw_a = 1; } else { saw_b = 1; }
                    continue;
                }

                *armed = 1;

                {
                    uint32_t seq = ((uint32_t)g_rxFrame[18] << 24) |
                                   ((uint32_t)g_rxFrame[19] << 16) |
                                   ((uint32_t)g_rxFrame[20] << 8)  |
                                    (uint32_t)g_rxFrame[21];

                    if (*last != 0u && seq <= *last)
                    {
                        PRINTF("ENET-LAB3 CORRUPT: PAYLOAD-REPLAY peer 0x%04x seq "
                               "%u <= last %u -- the RX path delivered a STALE BUFFER "
                               "(a valid frame, just not a NEW one)\r\n",
                               et, (unsigned)seq, (unsigned)*last);
                        continue;
                    }
                    *last = seq;
                }

                if (is_a) { saw_a = 1; } else { saw_b = 1; }
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
                PRINTF("ENET-LAB3 PASS #%u: saw BOTH peers -- 0x%04x %s, 0x%04x %s "
                       "(foreign frames ignored: %u)\r\n",
                       (unsigned)++pass_seq,
                       @PA@u, armed_a ? "VERIFIED" : "presence-only",
                       @PB@u, armed_b ? "VERIFIED" : "presence-only",
                       (unsigned)rx_foreign);
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

launch() { timeout 25 "$QEMU" $QARGS -kernel "$1" \
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
