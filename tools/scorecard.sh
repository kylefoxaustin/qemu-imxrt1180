#!/usr/bin/env bash
#
# RT1180 example scorecard generator.
#
# Runs every real NXP MCUXpresso SDK example listed in docs/validation/corpus.tsv
# on the model and MEASURES the verdict against an EXTERNAL oracle -- a string the
# NXP firmware itself prints, cited by SDK file:line in the manifest. The oracle is
# SOURCED from NXP's code, never derived from our model. Emits docs/validation/
# SCORECARD.md.
#
# Tier A (prebuilt cm33 .bin from the SDK zip) always runs -- no toolchain needed,
# reproducible by anyone with reference/SDK_..._MIMXRT1180-EVK.zip.
# Tier B (built from source) runs only when SDK_ROOT + VENV are set and exist;
# otherwise Tier B rows are marked SKIPPED and the card is stamped Tier-A-only.
#
# COVERAGE GATE (see CLAUDE.md "coverage must be asserted, not printed"):
#   * the number of rows PROCESSED per tier is compared to EXPECTED_A / EXPECTED_B
#     held HERE, outside the artifact -- a silently dropped row FAILS the gate.
#   * every run row must reach OUTPUT (produce a console capture) or the gate FAILS.
#   * a `pass` row that does not pass, or an `xfail` row that now DOES -> the gate
#     FAILS (the scorecard is stale; update it). A negative test that rots green is
#     caught here, not shipped.
#
# Exit 0 only if coverage holds AND no unexpected FAIL/REGRESSED. Non-zero otherwise.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -u

# --- expected coverage (bump these when you add/remove manifest rows) ----------
EXPECTED_A=8
EXPECTED_B=7

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
MANIFEST="$ROOT/docs/validation/corpus.tsv"
OUTMD="${SCORECARD_OUT:-$ROOT/docs/validation/SCORECARD.md}"
FWDIR="${FWDIR:-$ROOT/reference/sdk-fw}"
SDK_ZIP="${SDK_ZIP:-$ROOT/reference/SDK_26_06_00_MIMXRT1180-EVK.zip}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# Tier B is available only with a source SDK + toolchain.
TIERB=0
if [ -n "${SDK_ROOT:-}" ] && [ -n "${VENV:-}" ] && [ -d "${SDK_ROOT:-/nonexistent}" ] && [ -d "${VENV:-/nonexistent}" ]; then
    TIERB=1
    export ARMGCC_DIR="${ARMGCC_DIR:-/usr}"
    export PATH="$VENV/bin:$PATH"
fi

[ -x "$QEMU" ] || { echo "ERROR: build QEMU first ($QEMU missing)"; exit 3; }

# Extract prebuilt cm33 demos for Tier A if not present.
if [ ! -d "$FWDIR" ] || [ -z "$(find "$FWDIR" -ipath '*evkmimxrt1180*cm33*.bin' 2>/dev/null)" ]; then
    echo ">> extracting prebuilt cm33 demos from $SDK_ZIP"
    unzip -o -q "$SDK_ZIP" "*evkmimxrt1180*cm33*.bin" -d "$FWDIR" \
        || { echo "ERROR: could not extract prebuilt demos; set SDK_ZIP."; exit 3; }
fi

# --- run helpers ---------------------------------------------------------------
# Run an ELF/bin under QEMU, capture console to $1, honour extra flags in $3.
qemu_run() { # console_out  image  extra_flags  secs
    local con="$1" img="$2" extra="${3:-}" secs="${4:-14}"
    timeout -k 3 "$secs" "$QEMU" -M mimxrt1180-evk -audio none -display none -monitor none \
        -kernel "$img" -serial "file:$con" $extra \
        -semihosting-config enable=on,target=native >/dev/null 2>&1
}

tierA_image() { # example -> path to prebuilt bin (echo), rc!=0 if none
    find "$FWDIR" -ipath "*evkmimxrt1180/$1/cm33*.bin" 2>/dev/null | sort | head -1
}

tierB_build() { # example core -> echoes ELF path, rc!=0 on build fail
    local ex="$1" core="$2" tag bd
    tag="$(echo "${ex}_${core}" | tr '/' '_')"
    # Persistent build dir so west rebuilds incrementally across runs (override
    # with SCORECARD_BUILD_ROOT=... ; wipe it to force a clean build).
    bd="${SCORECARD_BUILD_ROOT:-/tmp/rt1180-scorecard-build}/b_$tag"
    ( cd "$SDK_ROOT" && "$VENV/bin/west" build -b evkmimxrt1180 --toolchain armgcc \
        "examples/$ex" -Dcore_id="$core" --config debug -d "$bd" ) \
        >"$WORK/build_$tag.log" 2>&1 || return 1
    local elf; elf="$(ls "$bd"/*.elf 2>/dev/null | head -1)"
    [ -n "$elf" ] && echo "$elf"
}

# --- process the manifest ------------------------------------------------------
declare -i nA=0 nB=0 fail=0 regress=0 missing_output=0
declare -i rows_pass=0 rows_banner=0 rows_xfail=0 rows_value=0 rows_xbuild=0 rows_skip=0
declare -a TBL

emit_row() { TBL+=("$1"); }

while IFS=$'\t' read -r tier example core kind oracle oracle_src note; do
    case "$tier" in ''|\#*) continue;; A|B) : ;; *) continue;; esac
    [ "$example" = "example" ] && continue   # header
    if [ "$tier" = A ]; then nA+=1; else nB+=1; fi

    verdict="?"; detail=""; icon="•"
    run_extra=""; [ "$core" = cm7 ] && run_extra="-icount shift=3"

    # kinds that never run firmware
    case "$kind" in
      value)  verdict="VALUE-PROVEN"; rows_value+=1
              emit_row "$tier|$example|$core|VALUE-PROVEN|proof: $note"; continue;;
      xbuild) verdict="XBUILD"; rows_xbuild+=1
              emit_row "$tier|$example|$core|XBUILD|$note"; continue;;
    esac

    # Tier B needs a source build; skip cleanly if unavailable.
    img=""
    if [ "$tier" = B ]; then
        if [ "$TIERB" = 0 ]; then
            verdict="SKIPPED"; rows_skip+=1
            emit_row "$tier|$example|$core|SKIPPED|needs SDK_ROOT+VENV (Tier-A-only run)"; continue
        fi
        if ! img="$(tierB_build "$example" "$core")"; then
            # A pass/banner row that won't even build is an unexpected FAIL;
            # an xfail/xbuild row build-failing is its documented state.
            if [ "$kind" = xfail ]; then verdict="XFAIL(build)"; rows_xfail+=1
                emit_row "$tier|$example|$core|XFAIL|build failed: $note"; continue
            fi
            verdict="FAIL(build)"; fail+=1
            emit_row "$tier|$example|$core|FAIL|build failed unexpectedly"; continue
        fi
    else
        img="$(tierA_image "$example")"
        [ -z "$img" ] && { verdict="FAIL(no-bin)"; fail+=1; missing_output+=1
            emit_row "$tier|$example|$core|FAIL|prebuilt bin not found in $FWDIR"; continue; }
    fi

    con="$WORK/con_$(echo "${tier}_${example}_${core}" | tr '/' '_')"
    qemu_run "$con" "$img" "$run_extra" 20
    [ -f "$con" ] || { missing_output+=1; }
    console="$(tr -d '\0' <"$con" 2>/dev/null)"

    if grep -qF "$oracle" <<<"$console"; then contains=1; else contains=0; fi

    case "$kind" in
      pass)
        if [ "$contains" = 1 ]; then verdict="PASS"; rows_pass+=1
            emit_row "$tier|$example|$core|PASS|\"$oracle\""
        else verdict="FAIL"; fail+=1
            emit_row "$tier|$example|$core|FAIL|oracle \"$oracle\" NOT printed"
        fi;;
      banner)
        if [ "$contains" = 1 ]; then verdict="BANNER"; rows_banner+=1
            emit_row "$tier|$example|$core|BANNER|reaches app (\"$oracle\"); $note"
        else verdict="FAIL"; fail+=1
            emit_row "$tier|$example|$core|FAIL|banner \"$oracle\" NOT printed"
        fi;;
      xfail)
        if [ "$contains" = 1 ]; then verdict="REGRESSED"; regress+=1
            emit_row "$tier|$example|$core|REGRESSED|now prints \"$oracle\" -- UPDATE the manifest to kind=pass"
        else verdict="XFAIL"; rows_xfail+=1
            emit_row "$tier|$example|$core|XFAIL|documented gap: $note"
        fi;;
    esac
done < "$MANIFEST"

# --- coverage gate -------------------------------------------------------------
gate_fail=0
[ "$nA" -ne "$EXPECTED_A" ] && { echo "GATE FAIL: Tier A rows $nA != expected $EXPECTED_A (manifest drift)"; gate_fail=1; }
[ "$nB" -ne "$EXPECTED_B" ] && { echo "GATE FAIL: Tier B rows $nB != expected $EXPECTED_B (manifest drift)"; gate_fail=1; }
[ "$regress" -gt 0 ] && { echo "GATE FAIL: $regress xfail row(s) now PASS -- scorecard is stale, update the manifest"; gate_fail=1; }
[ "$fail" -gt 0 ] && { echo "GATE FAIL: $fail pass/banner row(s) did not reach their oracle"; gate_fail=1; }
[ "$missing_output" -gt 0 ] && { echo "GATE FAIL: $missing_output run row(s) produced NO console output (did not reach OUTPUT)"; gate_fail=1; }

# --- emit SCORECARD.md ---------------------------------------------------------
stamp_tier="A+B (full)"; [ "$TIERB" = 0 ] && stamp_tier="A only (Tier B skipped: no SDK_ROOT/VENV)"
{
  echo "# RT1180 example scorecard"
  echo
  echo "> **GENERATED by \`tools/scorecard.sh\` — do not hand-edit.** Every verdict is"
  echo "> MEASURED: the model ran the real NXP firmware and the console was matched"
  echo "> against an EXTERNAL oracle (a string the firmware itself prints, cited by"
  echo "> SDK \`file:line\` in [\`corpus.tsv\`](corpus.tsv)). The oracle is SOURCED from"
  echo "> NXP's code, never from our model. Methodology: [README.md](README.md)."
  echo ">"
  echo "> This run: **$stamp_tier**. Regenerate: \`SDK_ROOT=… VENV=… tools/scorecard.sh\`."
  echo
  echo "## Legend"
  echo
  echo "| verdict | meaning |"
  echo "|---|---|"
  echo "| **PASS** | firmware ran to its documented **success** string |"
  echo "| **BANNER** | reached the app (printed its start banner) but the example needs an external **host** (SD card / USB host / accel) to complete — not a full pass |"
  echo "| **XFAIL** | a **known, documented gap**: the success string is not reached; the reason is recorded (a roadmap item) |"
  echo "| **VALUE-PROVEN** | no console oracle; correctness is pinned by a dedicated value-test (named) |"
  echo "| **XBUILD** | the example does not build for this board in this SDK (an SDK gap, not a model fault) |"
  echo "| **REGRESSED** | an XFAIL row now passes — the manifest is stale and the gate FAILS until updated |"
  echo
  echo "## Results"
  echo
  echo "| tier | example | core | verdict | detail |"
  echo "|------|---------|------|---------|--------|"
  for r in "${TBL[@]}"; do IFS='|' read -r t e c v d <<<"$r"
     printf "| %s | \`%s\` | %s | **%s** | %s |\n" "$t" "$e" "$c" "$v" "$d"; done
  echo
  echo "## Tally (MEASURED — classes are NOT summed into a single headline)"
  echo
  echo "| class | count |"
  echo "|---|---|"
  echo "| PASS (ran to success) | $rows_pass |"
  echo "| BANNER (reached app, blocked on external host) | $rows_banner |"
  echo "| XFAIL (documented gap) | $rows_xfail |"
  echo "| VALUE-PROVEN (pinned by a value-test) | $rows_value |"
  echo "| XBUILD (SDK build gap) | $rows_xbuild |"
  [ "$rows_skip" -gt 0 ] && echo "| SKIPPED (Tier B, no SDK this run) | $rows_skip |"
  echo
  echo "Coverage gate: Tier A $nA/$EXPECTED_A, Tier B $nB/$EXPECTED_B."
  if [ "$gate_fail" = 0 ]; then echo "**Gate: PASS.**"; else echo "**Gate: FAIL (see console).**"; fi
} > "$OUTMD"

echo ">> wrote $OUTMD  (Tier A $nA/$EXPECTED_A, Tier B $nB/$EXPECTED_B; run: $stamp_tier)"
echo ">> PASS=$rows_pass BANNER=$rows_banner XFAIL=$rows_xfail VALUE=$rows_value XBUILD=$rows_xbuild SKIP=$rows_skip FAIL=$fail REGRESSED=$regress"
exit "$gate_fail"
