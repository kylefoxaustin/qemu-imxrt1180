#!/usr/bin/env bash
# The registers no automated gate in this tree can watch: the RM prints "See section"
# in their reset column, so the extractor REFUSES them -- correctly.
# A REFUSAL IS NOT A CHECK.  These are hand-read from the RM, with receipts.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
export QEMU="${QEMU:-$DIR/../../build/qemu-system-arm}"
exec python3 "$DIR/check.py"
