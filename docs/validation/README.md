# RT1180 example scorecard — methodology

This directory holds the model's **example scorecard**: a tracked, regenerable
record of which real NXP MCUXpresso SDK examples run on the emulator, and *how far*
each one gets. It is the evidence behind the "What runs today" table in the top-level
[README](../../README.md).

> **Why this exists.** The previous `docs/validation/` was a verbatim copy from the
> `mcxn947qemu` sibling — a different chip — carrying an unbacked "34/47 byte-exact"
> figure and zero RT1180 examples. It was deleted (see repo history). This is the
> RT1180-specific replacement, built from scratch and **measured on this model**.

## The one rule: the oracle is SOURCED from NXP, never from us

A verdict is only trustworthy if the thing it is compared against comes from
**outside** the artifact under test. So every row's success criterion is an
**external oracle** — a string the NXP firmware *itself* prints — and the manifest
cites exactly where in the SDK source that string lives (`oracle_src`, as
`file:line`). We assert the firmware reached *its own* notion of success; we never
grade the model against a number the model produced.

This is the project's core guardrail applied to validation (see
[`CLAUDE.md`](../../CLAUDE.md)): *"a number with no expected value is a fact, not a
control,"* and *"anchor on the RM, the SDK's constants, or the firmware's own intent
— never on the thing under test."*

## What the verdicts mean

| verdict | it means |
|---|---|
| **PASS** | the firmware ran all the way to its documented **success** string |
| **BANNER** | it reached the app (printed its start banner) but the example then needs an external **host** — an SD card, a USB host, an on-board sensor — that a bare machine model does not provide. Honestly *not* a full pass. |
| **XFAIL** | a **known, documented gap**: the success string is not reached, and the manifest records why (each is a roadmap item). |
| **VALUE-PROVEN** | the example prints no usable console oracle (e.g. `led_blinky`, the cm7 FOC demo); its correctness is instead pinned by a dedicated value-test in `tests/`, named in the row. |
| **XBUILD** | the example does not build for `evkmimxrt1180` in this SDK — an SDK packaging gap, not a model fault. |
| **REGRESSED** | an XFAIL row now *passes*. This is good news that **fails the gate on purpose**: it means a documented gap has closed and the manifest must be promoted to `pass`. Prevents a negative test from silently rotting green as the model improves. |

## Two tiers

- **Tier A — prebuilt `.bin`** shipped inside the SDK zip. No toolchain needed;
  reproducible by anyone with `reference/SDK_26_06_00_MIMXRT1180-EVK.zip`.
- **Tier B — built from source** with `west` + `arm-none-eabi-gcc`, `--config debug`
  (links to TCM/RAM; see the top-level README for why). Exercises the unmodified
  `fsl_*` drivers. Runs only when `SDK_ROOT` + `VENV` are set.

## The coverage gate

Counting coverage is not asserting it. `tools/scorecard.sh` holds the **expected row
count per tier** (`EXPECTED_A`/`EXPECTED_B`) *in the script, outside the artifact*,
and **fails** if the manifest has drifted, if any run row produced no console output
(did not reach OUTPUT), if a `pass` row missed its oracle, or if an `xfail` row
regressed to passing. A silently dropped example can therefore never read as
"everything covered."

## Regenerate

```sh
./configure --target-list=arm-softmmu && make -j"$(nproc)"    # QEMU must be built
# Tier A only (no toolchain):
tools/scorecard.sh
# Full A+B (build heroes from source):
SDK_ROOT=~/.cache/rt1180-sdk/sdk/mcuxsdk VENV=~/.cache/rt1180-sdk/venv \
    ARMGCC_DIR=/usr tools/scorecard.sh
```

The generated card is [`SCORECARD.md`](SCORECARD.md); the manifest is
[`corpus.tsv`](corpus.tsv). To add an example: add a manifest row (with a real
`oracle` + `oracle_src`), bump `EXPECTED_A`/`EXPECTED_B`, and regenerate.
