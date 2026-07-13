#!/usr/bin/env bash
# Diff every register at reset against the REFERENCE MANUAL.
#
# THE ONLY GATE IN THIS TREE THAT THE MODEL CANNOT SATISFY BY AGREEING WITH ITSELF.
#
# Every other test here was written by whoever wrote the model, so it can agree with
# the model about something that is not true of the silicon.  We have the extreme
# case on the record: our eDMA channel registers sat at the WRONG ADDRESSES for the
# life of the model -- inherited from the MCXN947 it was adapted from -- and every
# eDMA test was green, and all sixteen mutations came back "caught", because the
# tests took their addresses FROM THE MODEL.  Mutation testing is blind to that BY
# CONSTRUCTION: mutate the model and the mirror moves with it.
#
# The golden here comes from two documents nobody in this repo wrote:
#   the RM's reset column  (what a register READS at reset)
#   the CMSIS headers      (WHERE that register lives)
#
# Regenerate the golden (only needed if the RM or the SDK changes):
#   pdftotext -f 1 -l 9999 ../../reference/IMXRT1180RM.pdf rm.txt
#   ./extract-rm-golden.py rm.txt rm-golden.json
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
export QEMU="${QEMU:-$DIR/../../build/qemu-system-arm}"
exec python3 "$DIR/check.py"
