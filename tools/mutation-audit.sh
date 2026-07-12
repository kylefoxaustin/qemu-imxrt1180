#!/usr/bin/env bash
#
# Mutation audit — does each test CATCH a corrupted model, or is it decoration?
#
# Method (mcxn947qemu, 2026-07-12; 7 of their 15 tier-A tests could not fail):
#   for each block we claim to verify:
#     go into the MODEL and corrupt the data it produces -- a PLAUSIBLE wrong
#     value, not a crash. Rebuild. Run only that block's test.
#     IF THE TEST STILL PASSES, THAT TEST CANNOT FAIL, and the capability we
#     claim for that block is asserted by nothing.
#   revert. next block.
#
# The diagnostic, which fits on one line:
#   GUARDED tests read a value back and compare it to an EXPECTED CONSTANT.
#   Anything that stops at "a flag set" or "an IRQ fired" is decoration.
#
# And the ladder (ollama_95_neutron, after finding this bug in SHIPPED SILICON --
# NXP's Neutron DMA-writes a WRONG tensor and reports success):
#   1. "did it ack?"                  -> finds nothing
#   2. "is the result buffer untouched?" -> finds a STUB
#   3. "is the result CORRECT vs a golden?" -> finds a real BUG
# Only rung 3 survives an engine that computes the wrong answer.
#
# Usage: tools/mutation-audit.sh          (runs all)
#        tools/mutation-audit.sh pwm      (one)
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ONLY="${1:-}"
BAK=$(mktemp -d); trap 'restore_all; rm -rf "$BAK"' EXIT

declare -A SRC TEST DESC PY
add() { SRC[$1]="$2"; TEST[$1]="$3"; DESC[$1]="$4"; PY[$1]="$5"; }

# --- the mutations: each returns a PLAUSIBLE wrong value ------------------
add pwm   hw/misc/imxrt1180_pwm.c   imxrt1180-pwm \
  "PWM period 3x too long (the FOC carrier frequency is silently wrong)" \
  't=t.replace("    return (uint16_t)(val1 - init) + 1u;   /* period in counter ticks */",
               "    return (uint16_t)(((val1 - init) + 1u) * 3u); /* MUTANT: 3x period */")'

add adc   hw/misc/imxrt1180_adc.c   imxrt1180-adc \
  "ADC conversion returns a plausible WRONG code (not the plant sample)" \
  't=t.replace("        uint16_t sample = s->channel_input[ch];                /* plant, or 0x8000 */",
               "        uint16_t sample = 0x1234; /* MUTANT: wrong conversion result */")'

add motor hw/misc/imxrt1180_motor.c imxrt1180-motor \
  "Motor plant reports 3x the phase current it computed" \
  't=t.replace("static uint16_t current_to_code(double i)\n{\n    double code = (double)M_ADC_MID + i * M_CUR_FS;",
               "static uint16_t current_to_code(double i)\n{\n    double code = (double)M_ADC_MID + 3.0 * i * M_CUR_FS; /* MUTANT */")'

add edma  hw/dma/imxrt1180_edma.c   imxrt1180-edma \
  "eDMA corrupts one byte of every transfer (CONTROL: this MUST be caught)" \
  't=t.replace("            address_space_write(&address_space_memory, daddr,",
               "            buf[0] ^= 0xFF; /* MUTANT */\n            address_space_write(&address_space_memory, daddr,", 1)'

restore_all() {
    for k in "${!SRC[@]}"; do
        [ -f "$BAK/$(basename "${SRC[$k]}")" ] && cp "$BAK/$(basename "${SRC[$k]}")" "${SRC[$k]}"
    done
    ninja -C build qemu-system-arm >/dev/null 2>&1
}

run_test() {   # $1 = test dir; echoes PASS or FAIL
    local o
    o=$( (cd "tests/$1" && timeout 40 make run 2>&1) )
    if echo "$o" | grep -q ": PASS"; then echo PASS; else echo FAIL; fi
}

printf "%-7s %-58s %-10s %s\n" "BLOCK" "MUTATION" "TEST SAYS" "VERDICT"
printf "%-7s %-58s %-10s %s\n" "-----" "--------" "---------" "-------"

blind=0
for k in pwm adc motor edma; do
    [ -n "$ONLY" ] && [ "$ONLY" != "$k" ] && continue
    src="${SRC[$k]}"
    cp "$src" "$BAK/$(basename "$src")"

    # baseline must be green, else the mutation proves nothing
    base=$(run_test "${TEST[$k]}")
    if [ "$base" != "PASS" ]; then
        printf "%-7s %-58s %-10s %s\n" "$k" "(baseline not green -- skipped)" "$base" "SKIP"
        continue
    fi

    python3 - "$src" <<PY
import sys
p = sys.argv[1]
t = open(p).read()
before = t
${PY[$k]}
if t == before:
    sys.stderr.write("MUTATION DID NOT APPLY\n"); sys.exit(2)
open(p, "w").write(t)
PY
    if [ $? -ne 0 ]; then
        printf "%-7s %-58s %-10s %s\n" "$k" "${DESC[$k]}" "-" "PATCH-FAIL"
        cp "$BAK/$(basename "$src")" "$src"; continue
    fi

    if ! ninja -C build qemu-system-arm >/dev/null 2>&1; then
        printf "%-7s %-58s %-10s %s\n" "$k" "${DESC[$k]}" "-" "BUILD-FAIL"
        cp "$BAK/$(basename "$src")" "$src"; ninja -C build qemu-system-arm >/dev/null 2>&1; continue
    fi

    res=$(run_test "${TEST[$k]}")
    if [ "$res" = "PASS" ]; then
        verdict="*** BLIND ***"; blind=$((blind+1))
    else
        verdict="caught"
    fi
    printf "%-7s %-58s %-10s %s\n" "$k" "${DESC[$k]}" "$res" "$verdict"

    cp "$BAK/$(basename "$src")" "$src"
    ninja -C build qemu-system-arm >/dev/null 2>&1
done

echo
if [ $blind -gt 0 ]; then
    echo "$blind test(s) CANNOT FAIL: they pass against a model that produces wrong data."
    echo "Those capabilities are asserted by nothing. Fix the TEST, and retract the claim first."
else
    echo "All mutations caught."
fi
