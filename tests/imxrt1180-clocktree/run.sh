#!/usr/bin/env bash
# CCM clock-tree VALUE golden: the exact Hz, not "a frequency appeared".
#
# The model used to STORE the MUX/DIV the guest wrote into CLOCK_ROOT[n].CONTROL and
# NEVER READ THEM BACK.  Every timer in the machine ticked at a hardcoded constant:
# GPT 10x slow, LPIT/TPM 5.5x slow, LPTMR 3.3x slow, QTMR 1.8x fast, eFlexPWM 1.5x
# fast.  Not one of the six defaults matched what the SDK's own CLOCK_GetRootClockFreq()
# computes -- and the FOC value-golden could not see it, because that golden checks
# AMPLITUDE (phase current to one ADC count) and the error was in TIME.
#
# The expected frequencies come from the SDK's constants and the EVK's clock_config.c,
# NOT from this model:  SYS_PLL2 = 24 MHz * 22 = 528 MHz; Bus_Aon = /4 = 132 MHz.
#
# Verdicts:  0 PASS   1 WRONG FREQUENCY   2 CANNOT TELL (the gate could not run)
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
export QEMU="${QEMU:-$DIR/../../build/qemu-system-arm}"
exec python3 "$DIR/check.py"
