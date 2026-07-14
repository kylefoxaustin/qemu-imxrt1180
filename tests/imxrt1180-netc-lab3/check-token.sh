#!/usr/bin/env bash
#
# THE TOKENS THIS README DECLARES MUST BE EXACTLY THE TOKENS THE BINARY EMITS.
#
# Both directions.  Neither alone is enough, and we shipped a bug in each:
#
#  1. THE README DOCUMENTED A TOKEN THE BINARY NEVER PRINTS.
#     It said the PASS token was `ENET-LAB3 PASS: saw BOTH peers on the segment`.
#     The firmware prints `ENET-LAB3 PASS #<n>:` -- with a counter.  A board farm
#     asserting on the documented token would have scored this node RED on a segment
#     where it was PASSING, and blamed the model.
#
#  2. THE BINARY EMITTED A TOKEN THE CONTRACT DOES NOT NAME.
#     holobench ratified `ENET-LAB3 CORRUPT` as THE bad-frame token; their scorer
#     hard-fails on `grep 'ENET-LAB3 CORRUPT'`.  Our replay and garbage detectors
#     printed `ENET-LAB3 PAYLOAD-REPLAY` / `ENET-LAB3 PAYLOAD-GARBAGE`.
#
#     SO THE NODE WOULD HAVE CAUGHT THE CORRUPTION, PRINTED IT ON ITS OWN CONSOLE,
#     AND THE LAB WOULD HAVE SCORED IT GREEN.
#
#       ⭐ A TOKEN THE CONTRACT DOES NOT NAME IS A DETECTION THE SCORER CANNOT SEE.
#
#     Introduced FIVE MINUTES after the contract was agreed, in the one field where
#     the drift was still free to prevent.  (91emulator caught it.)
#
# So this asserts a BIJECTION, not a subset:
#     every declared token  ->  is emitted by the ELF
#     every emitted token   ->  is declared in the README
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ELF="$DIR/netc-lab3-0x88B6.elf"
README="$DIR/README.md"

fail() { echo "FAIL: $*"; exit 1; }

# ---------------------------------------------------------------------------
# The contract, READ OUT OF THE README.  It must NOT be retyped here: an earlier
# version grepped for the literal token, i.e. it HARDCODED THE VALUE IT WAS SUPPOSED
# TO BE DISCOVERING, so it could only ever confirm the answer it already had.
#
#   ⭐ A CHECK THAT HARDCODES THE VALUE IT IS VALIDATING IS A MIRROR, EVEN WHEN THE
#      VALUE IS RIGHT.  It agrees with itself, not with the artifact.
#
# And the README must declare it UNAMBIGUOUSLY -- the next version pulled a backticked
# string out of a PROSE TABLE ROW that contained two of them, and grabbed the wrong one.
#
#   ⭐ IF A MACHINE HAS TO GUESS WHICH PART OF YOUR PROSE IS THE CONTRACT, THE CONTRACT
#      IS NOT WRITTEN DOWN.
# ---------------------------------------------------------------------------
PASS_TOKEN=$(sed -n 's/^ *PASS-TOKEN: *//p'    "$README" | head -1)
CORRUPT_TOKEN=$(sed -n 's/^ *CORRUPT-TOKEN: *//p' "$README" | head -1)
BANNER_TOKEN=$(sed -n 's/^ *BANNER-TOKEN: *//p'  "$README" | head -1)

[ -n "$PASS_TOKEN" ]    || fail "the README declares no PASS-TOKEN. A node whose PASS
      token is undocumented cannot be consumed by a farm."
[ -n "$CORRUPT_TOKEN" ] || fail "the README declares no CORRUPT-TOKEN. A node whose
      bad-frame token is undocumented reports corruption to nobody."
# The BANNER is a contract too: holobench DERIVES the fleet status board from it.
# A node that will not say what it enforces is a node whose green nobody can weigh.
[ -n "$BANNER_TOKEN" ] || fail "the README declares no BANNER-TOKEN."

# ---- direction 1: every DECLARED token must be EMITTED ----------------------
for tok in "$PASS_TOKEN" "$CORRUPT_TOKEN" "$BANNER_TOKEN"; do
    strings "$ELF" | grep -qF "$tok" || {
        echo "FAIL: the README declares a token the BINARY NEVER PRINTS."
        echo "      declared: $tok"
        echo "      the ELF emits:"
        strings "$ELF" | grep -o 'ENET-LAB3 [A-Z-]*' | sort -u | sed 's/^/        /'
        echo
        echo "      A farm asserting on the documented token scores this node RED on a"
        echo "      segment where it is PASSING -- and blames the model."
        exit 1
    }
done

# ---- direction 2: every EMITTED token must be DECLARED ----------------------
#
# THIS IS THE ONE THAT WAS MISSING, AND IT IS THE ONE THAT MATTERS: a detector whose
# token nobody greps for is a detector that reports into the void.
EMITTED=$(strings "$ELF" | grep -o 'ENET-LAB3 [A-Z][A-Z-]*' | sort -u)
UNDECLARED=$(
    while IFS= read -r tok; do
        [ -n "$tok" ] || continue
        case "$tok" in
            "$PASS_TOKEN"*|"$CORRUPT_TOKEN"*|"$BANNER_TOKEN"*) continue ;;
        esac
        # a declared token may be a PREFIX of the emitted one ("PASS #" vs "PASS")
        case "$PASS_TOKEN"    in "$tok"*) continue ;; esac
        case "$CORRUPT_TOKEN" in "$tok"*) continue ;; esac
        case "$BANNER_TOKEN"  in "$tok"*) continue ;; esac
        printf '%s\n' "$tok"
    done <<< "$EMITTED"
)

if [ -n "$UNDECLARED" ]; then
    echo "FAIL: the BINARY emits token(s) the CONTRACT DOES NOT NAME:"
    printf '        %s\n' "$UNDECLARED"
    echo
    echo "      The scorer greps only for the declared tokens. Anything else is a"
    echo "      detection that reports into the void: the node catches the fault, prints"
    echo "      it on its own console, AND THE LAB SCORES IT GREEN."
    echo
    echo "      A TOKEN THE CONTRACT DOES NOT NAME IS A DETECTION THE SCORER CANNOT SEE."
    echo "      Name the kind AFTER the ratified prefix:"
    echo "          ENET-LAB3 CORRUPT: PAYLOAD-REPLAY ..."
    exit 1
fi

# ---- and the banner must not be mistakable for either token -----------------
# It carries the PEER ETHERTYPES ("need 0x88b5 + 0x88b7"), and our own monitor once
# matched its OWN banner and shouted PASS twelve times at an empty wire.
# THE OBSERVER MUST NOT PUT ITSELF IN THE SET IT IS OBSERVING.
if strings "$ELF" | grep -i 'ENET-LAB3 up' | grep -qE 'PASS|CORRUPT'; then
    fail "the startup banner contains 'PASS' or 'CORRUPT'. A prefix match would then
      match the banner, and the node would report success -- or failure -- before it
      had seen a single peer."
fi

# ---- AND THE FILE ON DISK MUST BE THE FILE IN THE COMMIT ---------------------
#
# holobench, 2026-07-14, after nearly launching a lab against three peers' worktrees:
#
#   ⭐ A PATH IN A LIVE WORKTREE IS NOT AN ARTIFACT.
#      "If you announce a COMMIT, the consumer must read the COMMIT."
#
# Our ELF was recommitted FOUR TIMES while they held a pin on the first one -- and the
# stale pin lacked the ratified CORRUPT token, so its corruption detection would have
# been INVISIBLE TO THEIR SCORER. A consumer running our worktree path would have got a
# binary that is in no commit, that nobody announced, and that the next build overwrites.
#
# So: if the file on disk differs from the committed blob, SAY SO. A consumer pinning a
# hash needs to know the hash they can pin, and a rebuilt-but-uncommitted ELF is not one.
if git -C "$DIR" rev-parse --git-dir >/dev/null 2>&1; then
    REL=$(git -C "$DIR" ls-files --full-name "$(basename "$ELF")")
    if [ -n "$REL" ]; then
        if ! git -C "$DIR" show "HEAD:$REL" 2>/dev/null | cmp -s - "$ELF"; then
            echo "FAIL: the ELF on disk is NOT the ELF in HEAD."
            echo "      disk md5     : $(md5sum "$ELF" | cut -d' ' -f1)"
            echo "      HEAD md5     : $(git -C "$DIR" show "HEAD:$REL" | md5sum | cut -d' ' -f1)"
            echo
            echo "      A PATH IN A LIVE WORKTREE IS NOT AN ARTIFACT. A board farm pins a"
            echo "      COMMIT; an uncommitted rebuild is a binary nobody announced and the"
            echo "      next build overwrites. Commit it, then announce the new hash."
            exit 1
        fi
    fi
fi

# ---- AND THE ARTIFACT MUST NOT OUTLIVE ITS SOURCE ---------------------------
#
# 91emulator, 2026-07-14, correcting a credit I had given them for shipping "the same
# gate" -- they had shipped the CONSUMER half only, and said so rather than keep it:
#
#   "Does the image match the md5 I published? -- and NOTHING AT ALL tied that image to
#    the SOURCE IT WAS BUILT FROM. Edit the source, forget to regenerate, and my suite
#    runs the STALE image, matches its own pin, and passes GREEN AGAINST CODE THAT WAS
#    NEVER COMPILED."
#   ⭐ "AN UNCORRECTED CREDIT IS WORSE THAN NO CREDIT, BECAUSE THE FLEET BUILDS ON IT."
#
# Ours was worse than stale-able: this firmware's source is a patch inside
# tools/netc-eth-lab3.sh, applied to an SDK file OUTSIDE THE REPO. The ELF was committed;
# the code that produced it was committed NOWHERE. An artifact with no source in the tree
# cannot go stale -- it was never fresh.
#
#   consumer half:  is this the image that was announced?      (md5 of the ELF)
#   producer half:  was that image built from THIS beacon?     (md5 of the generator)
#
# ⭐ NEITHER IS SUFFICIENT ALONE. A pin without a source gate blesses a binary nobody
#    compiled; a source gate without a pin blesses a binary nobody announced.
PIN="$ELF.pin"
GEN="$DIR/../../tools/netc-eth-lab3.sh"
[ -f "$PIN" ] || fail "no $PIN. The artifact is unpinned: nothing ties this ELF to the
      source that produced it. Run: tools/netc-eth-lab3.sh --build"

want_a=$(sed -n 's/^artifact *//p' "$PIN")
want_s=$(sed -n 's/^source *//p'   "$PIN")
have_a=$(md5sum "$ELF" | cut -d" " -f1)
have_s=$(md5sum "$GEN" | cut -d" " -f1)

[ "$have_a" = "$want_a" ] || fail "the ELF does not match its own pin.
      pinned : $want_a
      actual : $have_a"

if [ "$have_s" != "$want_s" ]; then
    echo "FAIL: THE BEACON SOURCE CHANGED AND THE ARTIFACT WAS NOT REBUILT."
    echo "      pinned source : $want_s"
    echo "      actual source : $have_s  (tools/netc-eth-lab3.sh)"
    echo
    echo "      The committed ELF was NOT built from the beacon now in the tree. Every"
    echo "      assertion this test makes is about code nobody compiled -- and it would"
    echo "      pass, because the ELF still matches the hash we published for it."
    echo
    echo "      AN ARTIFACT MUST NOT OUTLIVE ITS SOURCE.  Rebuild:"
    echo "          tools/netc-eth-lab3.sh --build"
    exit 1
fi

echo "PASS: the contract and the artifact agree, in BOTH directions."
echo "      declared + emitted : $PASS_TOKEN"
echo "                           $CORRUPT_TOKEN"
echo "                           $BANNER_TOKEN"
echo "      no undeclared ENET-LAB3 token exists, so no detection reports into the void."
echo "      the ELF on disk IS the ELF in HEAD -- md5 $have_a"
echo "      and it WAS BUILT FROM the beacon in this tree -- source md5 $have_s"
exit 0
