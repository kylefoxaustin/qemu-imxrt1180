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
loop = '''    {
        uint32_t saw_a = 0, saw_b = 0, pass_seq = 0;
        uint32_t tx_seq = 0, last_a = 0, last_b = 0;
        static const uint8_t MY_MAC[6] = { %s };
        PRINTF("ENET-LAB3 up: rt1180 ethertype 0x%%04x, need 0x%%04x + 0x%%04x\\r\\n",
               0x%04Xu, 0x%04Xu, 0x%04Xu);

        /* BROADCAST FOREVER. A peer that is not here yet is not a failure. */
        for (;;)
        {
            /*
             * STAMP THE PAYLOAD. The lab's verdict keys on ETHERTYPE, and an
             * EtherType survives a corrupted frame body -- so a node can report
             * "I saw both peers" over garbage. (Ours did: 88 frames DMA'd to guest
             * address 0, and it PASSED.)  holobench, 2026-07-13: "the checkable
             * payload is the last thing between us and a lab that can be trusted
             * when it is green."
             *
             * ⚠ AND A CHECKSUM WOULD NOT CATCH IT. When the RX path drops a frame on
             * the floor it leaves the descriptor pointing at a STALE BUFFER -- which
             * holds a PREVIOUSLY VALID frame, with a PERFECTLY VALID CHECKSUM. The
             * corruption is not a mangled frame; it is an OLD one, delivered again.
             *
             * ⇒ A MONOTONIC SEQUENCE NUMBER. A stale buffer REPLAYS an old seq, and a
             *   replay goes BACKWARDS. Dropped frames make it jump FORWARD, which is
             *   fine and honest. Assert it strictly INCREASES, per peer.
             *   The same discipline as the heartbeat: ASSERT ON A NUMBER GOING UP.
             */
            g_txFrame[14] = 'L';  g_txFrame[15] = 'B';
            g_txFrame[16] = '3';  g_txFrame[17] = '!';
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
                (void)EP_ReceiveFrameCopy(&g_ep_handle, 0, g_rxFrame, length, NULL);
                uint16_t et = ((uint16_t)g_rxFrame[12] << 8) | g_rxFrame[13];

                /* RULE 1: our own broadcast comes back to us on a mcast socket.
                 * Counting it would "see a peer" that is ourselves. */
                if (et == 0x%04Xu) { continue; }

                /* RULE 1b: AND THE SAME DOOR, ON THE OTHER HINGE.
                 *
                 * I closed rule 1 against myself by EtherType and then never checked
                 * the SOURCE ADDRESS. When the RX ring overran, the driver copied a
                 * STALE buffer whose body was ours, and this node cheerfully reported
                 *     "peer ethertype 0x88b7 src 54:27:8d:00:00:00"
                 * -- a peer whose MAC was ITS OWN -- and PASSED. The assertion keyed on
                 * EtherType and was structurally incapable of seeing that the frame
                 * BODY was garbage.
                 *
                 * A FRAME THAT CLAIMS TO COME FROM ME DID NOT CROSS THE WIRE. Refuse it,
                 * whatever its EtherType says. This is a real assertion: it FAILED
                 * before the ring-full fix landed, and it is why the fix is verifiable. */
                if (g_rxFrame[6]  == MY_MAC[0] && g_rxFrame[7]  == MY_MAC[1] &&
                    g_rxFrame[8]  == MY_MAC[2] && g_rxFrame[9]  == MY_MAC[3] &&
                    g_rxFrame[10] == MY_MAC[4] && g_rxFrame[11] == MY_MAC[5])
                {
                    PRINTF("ENET-LAB3 CORRUPT: frame claims src = MY OWN MAC "
                           "(et 0x%%04x) -- RX path is lying\\r\\n", et);
                    continue;
                }

                /* PAYLOAD ASSERTION -- the frame BODY, not just its label. */
                if (g_rxFrame[14] != 'L' || g_rxFrame[15] != 'B' ||
                    g_rxFrame[16] != '3' || g_rxFrame[17] != '!')
                {
                    PRINTF("ENET-LAB3 PAYLOAD-GARBAGE: et 0x%%04x carries no beacon "
                           "magic -- the RX path handed up a buffer that is not a "
                           "beacon\\r\\n", et);
                    continue;
                }
                {
                    uint32_t seq = ((uint32_t)g_rxFrame[18] << 24) |
                                   ((uint32_t)g_rxFrame[19] << 16) |
                                   ((uint32_t)g_rxFrame[20] << 8)  |
                                    (uint32_t)g_rxFrame[21];
                    uint32_t *last = (et == 0x%04Xu) ? &last_a : &last_b;

                    /* A REPLAY IS THE SIGNATURE OF A STALE BUFFER. It cannot happen on
                     * a wire that delivers each frame once; it happens when the RX path
                     * drops a frame and leaves the descriptor pointing at the LAST one. */
                    if (*last != 0u && seq <= *last)
                    {
                        PRINTF("ENET-LAB3 PAYLOAD-REPLAY: peer 0x%%04x seq %%u <= last "
                               "%%u -- the RX path delivered a STALE BUFFER\\r\\n",
                               et, (unsigned)seq, (unsigned)*last);
                        continue;
                    }
                    *last = seq;
                }

                if (et == 0x%04Xu && !saw_a)
                {
                    saw_a = 1;
                    PRINTF("ENET-LAB3 rx: peer ethertype 0x%%04x src %%02x:%%02x:%%02x:%%02x:%%02x:%%02x\\r\\n",
                           et, g_rxFrame[6], g_rxFrame[7], g_rxFrame[8],
                           g_rxFrame[9], g_rxFrame[10], g_rxFrame[11]);
                }
                if (et == 0x%04Xu && !saw_b)
                {
                    saw_b = 1;
                    PRINTF("ENET-LAB3 rx: peer ethertype 0x%%04x src %%02x:%%02x:%%02x:%%02x:%%02x:%%02x\\r\\n",
                           et, g_rxFrame[6], g_rxFrame[7], g_rxFrame[8],
                           g_rxFrame[9], g_rxFrame[10], g_rxFrame[11]);
                }
            }

            /* RULE 3: PASS only on BOTH -- AND THEN RE-ARM, FOREVER.
             *
             * This used to latch (`announced = 1`, saw_a/saw_b never cleared). It
             * printed PASS once and then STOPPED LOOKING -- and a satisfied assertion
             * and an absent one print exactly the same thing: nothing.
             *
             * mcxn947qemu named the class and holobench measured it on us:
             *   "A COLLAPSED oracle never COULD see the axis. An EXPIRED oracle
             *    COULD, DID, and then STOPPED LOOKING."
             * In the fleet's staggered lab, all three nodes PASSed at t+210 -- and
             * from that instant two of the three were blind. The one node that
             * re-armed was the one scheduled to DEPART at t+420, so after the
             * departure ZERO assertions remained on a segment whose survivability
             * was the entire point of the lab.
             *
             * ⭐ A RE-ARMING ASSERTION IS AN ORACLE THAT CANNOT EXPIRE. Its PASS line
             * stops being a one-shot verdict and becomes a HEARTBEAT WITH THE WIRE IN
             * THE LOOP -- and a heartbeat that STOPS is something a scorer can assert
             * on POSITIVELY, instead of inferring health from silence.
             *
             * The sequence number makes it stronger still: "the wire absorbed the
             * loss" becomes an assertion on A NUMBER GOING UP, never on the absence
             * of a message. (mcxn's offer, taken.)
             */
            if (saw_a && saw_b)
            {
                PRINTF("ENET-LAB3 PASS #%%u: saw BOTH peers on the segment\\r\\n",
                       (unsigned)++pass_seq);
                saw_a = 0;
                saw_b = 0;          /* RE-ARM: go back to requiring BOTH, forever. */
            }
            for (volatile uint32_t d = 0; d < 400000U; d++) { }
        }
    }
''' % (", ".join("0x%02X" % b for b in mac), me, pa, pb, me, pa, pa, pb)
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
