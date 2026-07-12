# CLAUDE.md — NXP i.MX RT1180 QEMU machine model

> **2026-07-12: this file used to describe the MCXN947.** It was a verbatim copy
> from the `mcxn947qemu` project and had never been rewritten — so the one doc
> every session in this repo reads *first*, and which is injected as authoritative
> project instructions, pointed a newcomer at the wrong chip: wrong machine name,
> wrong file prefix, wrong console, wrong IRQ count, wrong memory map, and a
> status line that still said "build + smoke test ← start here".
>
> That is rung 5 of the fleet's audit ladder, and it is the one that bites hardest:
> **a rule does not die by being wrong, it dies by not arriving.** Everything below
> is derived from the code and the CMSIS header, not from the MCX.

## Mission

A **fidelity-first** QEMU machine model for the **NXP i.MX RT1180** crossover MCU
(Cortex-M33 secure boot core + Cortex-M7 main core). Machine: **`mimxrt1180-evk`**
(the MIMXRT1180-EVK, part MIMXRT1189). All model logic lives in `imxrt1180_*`
files; generic QEMU is not edited except where a device genuinely belongs upstream.

Two goals: **upstreamable quality**, and a **board-to-board node** for the fleet's
holobench. The frontier is **motor control** — a real FOC loop closing against a
virtual PMSM plant.

## Source of truth

- Device facts (bases, IRQs, register offsets, bit masks, **register counts**)
  come from the **MIMXRT1189 CMSIS headers**, in-repo at
  `reference/sdk-1189/mcuxsdk/devices/RT/RT1180/MIMXRT1189/` (`*_COMMON.h` and
  `periph/PERI_*.h`), and the RM.
- Driver *behaviour* comes from the MCUXpresso SDK's `fsl_*` drivers. **Model what
  the driver polls, not just what the RM lists** — the two are not the same, and
  the difference is where hangs live (see Lessons).

## Verified facts (MIMXRT1189)

| Property | Value |
|---|---|
| Cores | Cortex-M33 (boot/secure, prio-bits 3) + Cortex-M7 (main, prio-bits 4) |
| NVIC external IRQs | **239** (highest `ECAT_RST_OUT_IRQn` = 238) |
| Console | **LPUART1 @ `0x44380000`, IRQ 19** (`serial_hd(0)`) |
| B2B link port | LPUART2 @ `0x44390000`, IRQ 20 (`serial_hd(1)`) |
| Code TCM (M33) | `0x0FFE0000`, 128 KiB — where SDK `--config debug` images link |
| OCRAM1 | `0x20484000` |
| FlexSPI1 NOR (XIP) | `0x28000000`, 16 MiB — secure alias `0x38000000` |
| NETC (Ethernet) | `0x60000000` |

TrustZone-M: peripherals are aliased non-secure `0x4xxx_xxxx` / secure `0x5xxx_xxxx`.

## Build & run

```sh
./configure --target-list=arm-softmmu && make -j"$(nproc)"
./build/qemu-system-arm -M mimxrt1180-evk -kernel <fw.elf> \
    -serial mon:stdio -semihosting-config enable=on,target=native \
    -d unimp,guest_errors
```

## Running REAL NXP firmware (the gold standard)

The MCUXpresso SDK is in-repo (`reference/SDK_26_06_00_MIMXRT1180-EVK.zip`) and
extracted to a **reboot-persistent cache** at `~/.cache/rt1180-sdk/`
(`sdk/mcuxsdk` + `venv`). Build an example:

```sh
export PATH=~/.cache/rt1180-sdk/venv/bin:$PATH ARMGCC_DIR=/usr
cd ~/.cache/rt1180-sdk/sdk/mcuxsdk
west build -b evkmimxrt1180 --toolchain armgcc \
    examples/driver_examples/<path> -Dcore_id=cm33 --config debug -d /tmp/b
```

**`--config debug` is load-bearing.** The default (`flexspi_nor_debug`) builds XIP
images that assume the boot ROM set up FlexSPI; our `-kernel` loader doesn't
replicate that. `--config debug` links to TCM and runs directly. Harness:
`tools/sdk-run.sh`.

## Guardrails

- **Never fabricate register offsets, bases, IRQ numbers, or REGISTER COUNTS** —
  derive them from the CMSIS header or the RM. A wrong offset is a silent hang; a
  wrong *count* silently truncates data (S3MU modelled 4 TX registers; the real
  part has 8, so a long crypto message was cut in half on the way in).

- **Never report SUCCESS for something you did not compute. TELL THE GUEST.**
  Declining to model something is fine. Telling firmware it succeeded at something
  it never did is not.
  1. **Where does the result land?** If it lands in *a pointer the guest gave you*,
     then "acked + buffer untouched" is a silent-wrong that no status check and no
     IRQ-counting test will ever see. (The ELE `GET_RNG_RANDOM` bug hid exactly
     there: `kStatus_Success` over an un-written buffer — firmware would have
     seeded a crypto stack with un-computed data.)
  2. **Who gets told?** `LOG_UNIMP`/QMP reaches the **operator**. The firmware
     under test *cannot see it*. **An out-of-band flag is not an honest fault.**
  3. **Fault through the block's own documented, NON-GATING channel** — a status
     field, an error-trap IRQ. Never by withholding the completion: that *hangs*
     the driver instead of *informing* it.
  4. **Compute it if you can.** Decline only what you genuinely cannot produce —
     entropy you *can* produce, so ELE RNG returns real `qemu_guest_getrandom`
     bytes rather than an honest refusal.
  5. Any escape hatch restoring a fabricated success is a **property, default OFF**.
  6. **Go read your own rule.** If a doc calls host-side flagging an acceptable
     endpoint, the policy is the bug and it will regenerate the code bug after you
     fix it. See the `(flagged)` definition in `PERIPHERALS.md`.

- **A test that cannot fail is decoration, and you cannot tell by reading it.**
  Break the model on purpose and demand the test notice: **`tools/mutation-audit.sh`**.
  - Guarded tests read a value back and compare it to an **independently-derived
    expected value**. "An IRQ fired" / "a VALID bit set" / "it's in range" proves
    nothing — **a range is not a golden** (it hides a 3×-wrong value *and* an
    unconverged one).
  - **One shape is not a golden.** Sweep the axis the *register* exposes, not the
    one your firmware happens to use. (A PWM golden at one prescaler passed a model
    with the modulo hardwired.)
  - **Prove your negative test can fail**, and re-prove it: an unmeasured threshold
    is a decoration, and a negative test rots green when the model improves under it.

- **Retract before you fix.** A false claim must not stay up while you work. See
  the retraction blocks in `PERIPHERALS.md`.

## Bring-up loop

Run real firmware with `-d unimp,guest_errors`; the log is the prioritised to-do
list. Model the first thing it blocks on, connect its IRQ via
`qdev_get_gpio_in(armv7m, <IRQn>)`, rebuild, repeat.

## Status

Broad peripheral coverage (see `PERIPHERALS.md` for the per-block table and the
honest gaps). Working today:

- **Real NXP SDK firmware runs** — 33/47 `driver_examples` byte-exact against the
  unmodified `fsl_*` drivers.
- **FOC frontier**: eFlexPWM + EQDC + LPADC + PWM→XBAR→ADC sync + a calibrated dq
  PMSM plant. Value-verified against first-principles goldens (phase current
  matches Ohm's law to one ADC count).
- **Ethernet (NETC/ENETC)**: real L2 over a QEMU socket netdev; 1180↔1180 verified
  byte-exact.
- **FlexSPI NOR**: `rom_device` XIP window + a real `m25p80`; storage-write-verified
  (erase→program→read-back byte-exact, and program-without-erase only clears bits).
- **B2B**: UART and Ethernet transports live.

Open: the 3-node raw-L2 segment with mcxn947qemu + 95emulator (our node is
`0x88B6` on mcast `230.0.0.9:31337`); NETC L2 switch path; ASRC/audio; LPADC A/B
side mux (blocks the stock `mc_pmsm` FOC demo).

## Fleet

Sibling QEMU models (`mcxn947qemu`, `93emulator`, `95emulator`, `91emulator`) share
findings over a message bus (`~/.claude/bin/bus.sh`). Several of this model's worst
bugs were found by a peer's audit and vice-versa — **when you write the model and
the test, you unconsciously test the model you wrote.**
