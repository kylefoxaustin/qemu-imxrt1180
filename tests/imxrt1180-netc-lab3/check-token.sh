#!/usr/bin/env bash
#
# THE TOKEN THIS README DOCUMENTS MUST EXIST IN THE BINARY IT DOCUMENTS.
#
# This node is consumed by holobench, a board farm, which asserts on the node's own
# PASS token -- VERBATIM, taken from the README.  That is the correct discipline
# ("grep the token, not the substring").
#
# And it broke, because nothing checked that the README and the ELF agreed:
#
#     README said :  ENET-LAB3 PASS: saw BOTH peers on the segment
#     the ELF says:  ENET-LAB3 PASS #%u: saw BOTH peers on the segment
#
# THE DOCUMENTED STRING DID NOT EXIST IN THE BINARY.  A farm asserting on it would
# have scored this node RED on a segment where it was PASSING -- and the failure
# would have been blamed on the model, not on the doc.
#
#   ⭐ A PASS TOKEN IS AN INTERFACE.  IT WAS DOCUMENTED IN ONE FILE AND EMITTED FROM
#      ANOTHER, AND NOBODY DIFFED THEM.
#
# So: diff them, every run.  The README declares the token; this asserts the ELF
# actually emits it.  Consumers of this node are downstream of BOTH.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ELF="$DIR/netc-lab3-0x88B6.elf"
README="$DIR/README.md"

# The token the README tells the world to match, READ OUT OF THE README.
#
# It must NOT be retyped here.  My first version of this check grepped for the literal
# string `ENET-LAB3 PASS #` -- i.e. it HARDCODED THE TOKEN IT WAS SUPPOSED TO BE
# DISCOVERING, so it could only ever confirm the answer it already had.  Edit the
# README's token to something the binary does not print and that check said... nothing.
#
#   ⭐ A CHECK THAT HARDCODES THE VALUE IT IS VALIDATING IS A MIRROR, EVEN WHEN THE
#      VALUE IS RIGHT.  It agrees with itself, not with the artifact.
#
# So: pull whatever is in backticks on the README's `PASS token` row, and hold the ELF
# to THAT.  Change the README and this check follows it -- which is the whole point.
# ...and the README must declare it UNAMBIGUOUSLY.  My first extraction pulled a
# backticked string out of a prose table row -- and that row contained TWO of them (the
# token, and an illustrative full line), so the check grabbed the wrong one and failed
# on a correct README.
#
#   ⭐ IF A MACHINE HAS TO GUESS WHICH PART OF YOUR PROSE IS THE CONTRACT, THE CONTRACT
#      IS NOT WRITTEN DOWN.
#
# So the README carries ONE machine-readable line and this reads exactly that:
#     PASS-TOKEN: ENET-LAB3 PASS #
TOKEN=$(sed -n 's/^ *PASS-TOKEN: *//p' "$README" | head -1)

if [ -z "$TOKEN" ]; then
    echo "FAIL: the README does not declare a PASS token at all."
    echo "      A node whose PASS token is undocumented cannot be consumed by a farm."
    exit 1
fi

if ! strings "$ELF" | grep -qF "$TOKEN"; then
    echo "FAIL: the README documents a PASS token the BINARY NEVER PRINTS."
    echo "      README declares : $TOKEN"
    echo "      the ELF contains:"
    strings "$ELF" | grep -i 'ENET-LAB3 PASS' | sed 's/^/        /'
    echo
    echo "      A board farm asserting on the documented token scores this node RED"
    echo "      on a segment where it is PASSING -- and blames the model."
    exit 1
fi

# And the BANNER must not be mistakable for the PASS line: it carries the peer
# EtherTypes, and a monitor that greps for a peer ID matches its OWN banner.
# (It did. Twelve times. At an empty wire.)
if strings "$ELF" | grep -i 'ENET-LAB3 up' | grep -q 'PASS'; then
    echo "FAIL: the startup banner contains the string 'PASS'."
    echo "      A prefix match on the PASS token would then match the banner, and the"
    echo "      node would report success before it had seen a single peer."
    exit 1
fi

echo "PASS: the README's PASS token ($TOKEN) is emitted by the binary,"
echo "      and the startup banner cannot be mistaken for it."
exit 0
