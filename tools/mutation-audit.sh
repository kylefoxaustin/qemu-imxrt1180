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

add pwmsh hw/misc/imxrt1180_pwm.c   imxrt1180-pwm \
  "PWM ignores VAL1/INIT: modulo hardwired to 1000 (RIGHT at one shape, wrong elsewhere)" \
  't=t.replace("    return (uint16_t)(val1 - init) + 1u;   /* period in counter ticks */",
               "    (void)init; (void)val1; return 1000u; /* MUTANT: modulo hardwired */")'

add eqdc  hw/misc/imxrt1180_eqdc.c  imxrt1180-motor \
  "EQDC reports a plausible WRONG rotor position (offset by 40 counts)" \
  't=t.replace("    REG(s, R_LPOS) = pos & 0xFFFF;",
               "    pos += 40; /* MUTANT: wrong rotor position */\n    REG(s, R_LPOS) = pos & 0xFFFF;")'

add ele   hw/misc/imxrt1180_s3mu.c  imxrt1180-ele \
  "ELE fakes SUCCESS for un-computed crypto (the guest-lying bug)" \
  't=t.replace("        s->rr[1] = RESPONSE_FAILURE;", "        s->rr[1] = RESPONSE_SUCCESS; /* MUTANT: lie to the guest */")'

# --- peripheral-triggered eDMA: the request path, not the START path -------
#
# The mem-to-mem `edma` test above CANNOT reach any of these: it triggers with
# TCD_CSR[START], so it passes on a model where ERQ is a dead constant, no
# peripheral drives a request line, and CH_MUX selects nothing -- which is
# EXACTLY what this model was until 2026-07-12. A whole trigger path can be
# missing while its block's test sits green. (mcxn947qemu found this across the
# fleet with `grep -n 'ERQ\|DMAMUX\|dma_req' your_dma.c`.)

add dmaerq   hw/dma/imxrt1180_edma.c   imxrt1180-dmareq \
  "eDMA ignores CH_CSR[ERQ] -- an un-armed channel services hardware requests" \
  't=t.replace("""            if (!(c->csr & CH_CSR_ERQ)) {
                continue;
            }
            if (src == 0 || src >= IMXRT1180_EDMA_NUM_REQ || !s->req[src]) {""",
               """            if (src == 0 || src >= IMXRT1180_EDMA_NUM_REQ || !s->req[src]) { /* MUTANT: ERQ gate gone */""", 1)'

add dmaline  hw/dma/imxrt1180_edma.c   imxrt1180-dmareq \
  "eDMA ignores the peripheral request LINE -- arming a channel runs it" \
  't=t.replace("""            if (src == 0 || src >= IMXRT1180_EDMA_NUM_REQ || !s->req[src]) {
                continue;
            }""",
               """            if (src == 0 || src >= IMXRT1180_EDMA_NUM_REQ) { /* MUTANT: no line check */
                continue;
            }""", 1)'

add dmainl  hw/dma/imxrt1180_edma.c   imxrt1180-dmareq \
  "eDMA services requests INLINE from the peripheral MMIO handler (re-entrancy)" \
  't=t.replace("""    if (level) {
        qemu_bh_schedule(s->bh);   /* service OUTSIDE this MMIO dispatch */
    }""",
               """    if (level) {
        edma_service_bh(s);        /* MUTANT: re-entrant MMIO, silently dropped */
    }""", 1)'

add dmamaj  hw/dma/imxrt1180_edma.c   imxrt1180-dmareq \
  "one request runs the WHOLE major loop, not one minor loop (RX copies stale bytes)" \
  't=t.replace("""            (void)edma_minor_loop(s, n);
            progressed = true;""",
               """            while (!edma_minor_loop(s, n)) { /* MUTANT: drain whole major loop */
            }
            progressed = true;""", 1)'

add uartdma hw/char/imxrt1180_lpuart.c imxrt1180-dmareq \
  "LPUART holds its RX request line asserted regardless of RDMAE/RDRF" \
  't=t.replace("    qemu_set_irq(s->dma_rx_req, (s->baud & BAUD_RDMAE) && s->rx_full);",
               "    qemu_set_irq(s->dma_rx_req, 1);   /* MUTANT: line never drops */", 1)'

# --- TCD_CSR[START] is a SERVICE REQUEST (one minor loop), not "run the major
#     loop". This was WRONG in the model for months and NO TEST COULD SEE IT,
#     because every eDMA test used CITER=1 -- where one minor loop IS the whole
#     major loop and the two models are bit-for-bit indistinguishable. So did the
#     stock NXP edma4/memory_to_memory example (minorLoopBytes = the whole buffer).
#     tests/imxrt1180-edma now has a CITER=4 phase. (mcxn947qemu, 2026-07-12.)

add start   hw/dma/imxrt1180_edma.c   imxrt1180-edma \
  "TCD_CSR[START] drains the WHOLE major loop instead of one minor loop" \
  't=t.replace("""static void edma_start(IMXRT1180EDMAState *s, int n)
{
    (void)edma_minor_loop(s, n);
}""",
               """static void edma_start(IMXRT1180EDMAState *s, int n)
{
    unsigned guard = 0;
    while (!edma_minor_loop(s, n) && ++guard < 100000) { /* MUTANT: whole major loop */
    }
}""", 1)'

add startclr hw/dma/imxrt1180_edma.c  imxrt1180-edma \
  "TCD_CSR[START] is not auto-cleared when the channel executes (RM 5.5.4)" \
  't=t.replace("            c->tcd_csr &= ~TCD_CSR_START;",
               "            /* MUTANT: START left set */", 1)'

add edma  hw/dma/imxrt1180_edma.c   imxrt1180-edma \
  "eDMA corrupts one byte of every transfer (CONTROL: this MUST be caught)" \
  't=t.replace("        address_space_write(&address_space_memory, c->tcd_daddr,",
               "        buf[0] ^= 0xFF; /* MUTANT */\n        address_space_write(&address_space_memory, c->tcd_daddr,", 1)'

restore_all() {
    for k in "${!SRC[@]}"; do
        [ -f "$BAK/$(basename "${SRC[$k]}")" ] && cp "$BAK/$(basename "${SRC[$k]}")" "${SRC[$k]}"
    done
    ninja -C build qemu-system-arm >/dev/null 2>&1
}

run_test() {   # $1 = test dir; echoes PASS or FAIL
    #
    # FAIL WINS. This used to be `grep -q ": PASS"` -- i.e. "does a PASS token
    # appear ANYWHERE", not "did the test pass". Tests that report per-check
    # verdicts print BOTH (the ELE test under a lying model prints
    # "ELE: PASS - real entropy" AND "ELE: FAIL - HASH computed no digest"), so
    # the old check scored a FAILING run as PASS -- and the harness would then
    # declare BLIND a test that had in fact CAUGHT the mutation. A false
    # accusation against a correct test, which is worse than a missed one.
    # (orb_slam, 2026-07-12: "rung 2 wearing rung 3's clothes -- it tests whether
    # the token APPEARS when the property that matters is whether it is CORRECT,
    # and it feels identical from the inside.")
    #
    local o
    o=$( (cd "tests/$1" && timeout 60 make run 2>&1) )
    if echo "$o" | grep -q ": FAIL"; then echo FAIL
    elif echo "$o" | grep -q ": PASS"; then echo PASS
    else echo FAIL; fi
}

# COVERAGE, STATED UP FRONT. "All mutations caught" is only ever true of the
# blocks a mutation was actually POINTED AT. Anything not in the table below has
# NEVER been mutation-tested and its capability claim is UNVERIFIED by this tool,
# no matter how green its test looks. Do not let this table's PASS line imply a
# coverage it does not have -- that is the same lie one level up.
swept=$(grep -c '^add ' "$0")
total=$(ls -d "$ROOT"/tests/imxrt1180-*/ 2>/dev/null | wc -l)
echo "coverage: $swept of $total test directories have a mutation pointed at them."
echo "          the rest are UNVERIFIED by this tool. See the list in the README of this file."
echo
printf "%-7s %-58s %-10s %s\n" "BLOCK" "MUTATION" "TEST SAYS" "VERDICT"
printf "%-7s %-58s %-10s %s\n" "-----" "--------" "---------" "-------"

blind=0
notrun=0
ran=0
for k in pwm pwmsh adc motor eqdc ele edma start startclr dmaerq dmaline dmainl dmamaj uartdma; do
    [ -n "$ONLY" ] && [ "$ONLY" != "$k" ] && continue
    src="${SRC[$k]}"
    cp "$src" "$BAK/$(basename "$src")"

    # baseline must be green, else the mutation proves nothing
    base=$(run_test "${TEST[$k]}")
    if [ "$base" != "PASS" ]; then
        printf "%-7s %-58s %-10s %s\n" "$k" "(baseline not green -- skipped)" "$base" "SKIP"
        notrun=$((notrun+1))
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
        notrun=$((notrun+1))
        cp "$BAK/$(basename "$src")" "$src"; continue
    fi

    if ! ninja -C build qemu-system-arm >/dev/null 2>&1; then
        printf "%-7s %-58s %-10s %s\n" "$k" "${DESC[$k]}" "-" "BUILD-FAIL"
        notrun=$((notrun+1))
        cp "$BAK/$(basename "$src")" "$src"; ninja -C build qemu-system-arm >/dev/null 2>&1; continue
    fi

    res=$(run_test "${TEST[$k]}")
    ran=$((ran+1))
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
#
# A MUTATION THAT NEVER APPLIED LOOKS EXACTLY LIKE A CAUGHT ONE.
# (ollama_95_neutron, 2026-07-12: their sed matched nothing, the file was
# unchanged, the suite went green, and they scored a PASS they had not earned --
# "the tool I built to catch the disease had the disease". Mine had the same hole
# ONE LAYER UP: the ROW said PATCH-FAIL while the SUMMARY still said "all caught",
# and a human or a CI reading only the last line would have taken the green.)
# So: if any mutation did not take, this tool must NOT report success.
#
if [ $notrun -gt 0 ]; then
    echo "$notrun mutation(s) NEVER RAN (PATCH-FAIL / BUILD-FAIL / baseline red)."
    echo "A mutation that did not apply is NOT a mutation that was caught."
    echo "This run proves NOTHING about those blocks. Fix the anchors and re-run."
    exit 1
fi
if [ $blind -gt 0 ]; then
    echo "$blind test(s) CANNOT FAIL: they pass against a model that produces wrong data."
    echo "Those capabilities are asserted by nothing. Fix the TEST, and retract the claim first."
else
    #
    # SAY WHAT RAN, NOT WHAT THE TABLE CONTAINS.
    #
    # This used to print "All $swept swept blocks caught their mutation" -- the
    # size of the TABLE -- even when invoked as `mutation-audit.sh edma`, which
    # runs exactly ONE. So a filtered run of a single mutation announced that all
    # twelve blocks were guarded. The row was honest and the BOTTOM LINE was not,
    # which is the same shape as the PATCH-FAIL bug directly above: the summary is
    # the only line most readers (and every CI) actually consume.
    #
    echo "All $ran mutation(s) that RAN were caught."
    if [ -n "$ONLY" ]; then
        echo
        echo "FILTERED RUN ('$ONLY'). The other $((swept - ran)) mutation(s) in the table"
        echo "DID NOT RUN and this says NOTHING about them. Run with no argument for the suite."
        exit 0
    fi
    echo
    echo "THIS IS NOT 'THE SUITE IS GUARDED'. It is '$swept of $total blocks are'."
    echo "The unswept ones are UNVERIFIED -- a green test there proves nothing yet:"
    for d in "$ROOT"/tests/imxrt1180-*/; do
        n=$(basename "$d" | sed 's/imxrt1180-//')
        grep -q " imxrt1180-$n " "$0" || printf "    %s\n" "$n"
    done
fi
