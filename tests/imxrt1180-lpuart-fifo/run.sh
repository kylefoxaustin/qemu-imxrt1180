#!/usr/bin/env bash
# The LPUART RX FIFO, and the watermark nothing in this tree ever set.
# PARAM and FIFO advertise 16 deep; the model delivered ONE. A capability register is a
# contract, and it was never called in -- because RDRF is defined against RXWATER, and
# RXWATER resets to ZERO, so a 1-deep receiver and a 16-deep one look identical until
# somebody sets a watermark.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
export QEMU="${QEMU:-$DIR/../../build/qemu-system-arm}"
exec python3 "$DIR/check.py"
