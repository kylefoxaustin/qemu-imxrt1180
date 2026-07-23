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
  7. **GREP YOUR OWN GUARDRAILS. THEY DO NOTHING UNTIL YOU DO.** This rule had been
     written here for *months* while the code disobeyed it. One grep, four minutes:
     ```sh
     grep -rniE 'plausible|nominal|approximat|arbitrar|guess|fabricat|invent|placeholder|dummy|fake' hw/ include/
     ```
     65 hits. Most were the policy *working* ("flagged, not faked"). Several were
     confessions — including CCM's `OBSERVE.FREQUENCY_CURRENT`, which returned a
     **fabricated 6 MHz** into `CLOCK_GetFreqFromObs()`, i.e. straight into the
     guest's baud-rate arithmetic. Its "flag" was **a C comment. Firmware cannot
     read C comments.** *A constraint that does not reach the point of use is worse
     than no constraint, because you believe you are covered.* (ollama_95_neutron)

- **A `?:` IS NOT A SAFETY NET — IT IS A PLACE FOR A BUG TO LIVE WHERE NO TEST WILL
  LOOK.** Six timer blocks opened `if (!s->clk) { s->clk = DEFAULT; }`. The SoC drove
  **no** peripheral clock, and four of the six did not even *expose* a property to
  drive — so the guard made them look wirable while guaranteeing they never were.
  Every timer ran at a hardcoded constant and **not one of the six was right** (GPT
  10× slow … eFlexPWM 1.5× fast). Ask of every default: **when its input is missing,
  is that outcome DISTINGUISHABLE from the input being fine?** If not, it is
  camouflage. Where the spec is silent, pick the value that **exposes** the caller's
  mistake, not the one that absorbs it: a stopped clock is diagnosed in a minute, a
  *plausible* clock ships into somebody's product.

- **COVERAGE MUST BE ASSERTED, NOT PRINTED. A number with no expected value is a
  fact, not a control.** The reset-value gate published `unmatched in CMSIS: 4371`
  against a golden of 2892 — *it was blind to more registers than it checked, said so
  in bold on every run, and returned `PASS`*. Counting your coverage is not enough
  (that fix is mcxn947qemu's, and I shipped it, and it did not save me): the count
  must be **compared to an expected value held outside the artifact** and must FAIL
  the gate. Assert that a *class* reached the OUTPUT, not merely that the parser
  could READ it — and remember that **an entry which vanished from the oracle is
  indistinguishable from one that passed, unless you ask whether it was looked at.**

- **A test that cannot fail is decoration, and you cannot tell by reading it.**
  Break the model on purpose and demand the test notice: **`tools/mutation-audit.sh`**.
  - Guarded tests read a value back and compare it to an **independently-derived
    expected value**. "An IRQ fired" / "a VALID bit set" / "it's in range" proves
    nothing — **a range is not a golden** (it hides a 3×-wrong value *and* an
    unconverged one).
  - **A MIRROR THAT DECLARES ITSELF IS STILL A MIRROR.** `tests/imxrt1180-pwm`
    asserted the carrier *period*, swept prescaler **and** modulo, ±1% under
    `-icount` — the good kind of test. **It passed a model whose PWM clock was 1.5×
    wrong**, because `PWM_HZ` was taken *from the model*, and its own comment said
    so: *"if PWM_CLK were wrong, this golden would be wrong in exactly the same
    direction and still pass."* It was. It did. **Naming the hole in a comment does
    not close it — the flag discharges the anxiety and the gap stays.** Ask of every
    golden: *which of these numbers did the model give me?* Anchor on the RM, the
    SDK's constants, or the firmware's own intent — never on the thing under test.
  - **One shape is not a golden.** Sweep the axis the *register* exposes, not the
    one your firmware happens to use. (A PWM golden at one prescaler passed a model
    with the modulo hardwired.)
  - **Prove your negative test can fail**, and re-prove it: an unmeasured threshold
    is a decoration, and a negative test rots green when the model improves under it.
  - **A GREEN FROM A STALE BINARY IS A WEAK ORACLE WEARING A DISGUISE.** When you
    mutate BY HAND (not via `mutation-audit.sh`, which gates on the ninja exit code
    at line ~230), the trap is that your break does not compile — `-Werror` on an
    unused function, a type mismatch — the build FAILS, and your test then runs
    against the *last good* binary and comes back green. That green is
    indistinguishable from "the test can't catch the mutation." Both bit this model
    in one session: an unused-function `-Werror` after deleting a call, and a stale
    QEMU still holding gdb's `:1234` so the debugger attached to a pre-rebuild
    image. **Gate every by-hand post-mutation run on a CONFIRMED rebuild** — see the
    `[N/N] Linking target` line (or `ninja: no work to do` only when nothing
    changed), and kill stale background QEMU/gdbstubs first. A red you did not prove
    came from the *new* binary proves nothing. (95emulator, 2026-07-18.)
    - **`pkill -f <pattern>` MATCHES ITS OWN COMMAND LINE AND SIGKILLS ITS OWN SHELL.**
      A three-way-confirmed trap (ollama_95_neutron on an NPU release, 91emulator, and
      this model — 2026-07-19). `pkill -f 'build/qemu-system-arm'` run *inside* a
      background launch whose own argv contains that string kills itself before QEMU
      starts; it surfaces as a bare `exit 1`/`exit 144` (or, over ssh, reads as
      *network flakiness* on a wired link). Same family as the ghost gdbstub on :1234:
      **the tool you use to control the experiment is itself a participant in it.**
      Kill by PID (`for p in $(pgrep -f ...); do kill -9 $p; done`) or make the pattern
      un-self-matching; never put a broad `pkill -f` in the same command line it would
      match.
    - **BINARY FRESHNESS ≠ SOURCE PROVENANCE (for releases/patch prep).** The rebuild
      gate proves the artifact changed; it does NOT prove it was built from the source
      you *meant*. For a mutation audit the two collapse (you just edited the file); for
      a release or an upstream patch series they separate — "a confirmed rebuild of the
      WRONG source is the same lie one level up," and every downstream check still
      passes. There, also assert the INPUT's identity (sha of the source vs. the
      intended final) *before* building. (ollama_95_neutron, 2026-07-19.)

- **Retract before you fix.** A false claim must not stay up while you work. See
  the retraction blocks in `PERIPHERALS.md`.

## Bring-up loop

Run real firmware with `-d unimp,guest_errors`; the log is the prioritised to-do
list. Model the first thing it blocks on, connect its IRQ via
`qdev_get_gpio_in(armv7m, <IRQn>)`, rebuild, repeat.

## Status

Broad peripheral coverage (see `PERIPHERALS.md` for the per-block table and the
honest gaps). Working today:

- **Real NXP SDK firmware runs** against the unmodified `fsl_*` drivers — from
  `hello_world` through the audio, motor-control (cm7 `mc_pmsm`), and NETC-switch
  (`netc_switch`, end-to-end) demos; see the README table + `tests/imxrt1180-corpus`.
  (NOTE: the old "34/47 byte-exact" figure was unbacked — no tracked RT1180 example
  scorecard exists; `docs/validation/*` was a stale MCXN947 copy, since removed.
  Re-establishing an RT1180-specific scorecard is roadmap #3.)
- **Audio streaming**: the stock `sai/edma_transfer` demo runs end-to-end — AUDIO
  PLL + WM8962 codec + SAI1-master + eDMA stream the SDK's `music[]` sine to a wav,
  a mathematically-exact 1 kHz tone at 48 kHz.
- **DMA request lines**: SAI/LPSPI/LPI2C/LPADC/eFlexPWM all drive the eDMA hardware
  request path (every trigger shape, both controllers), each gate mutation-proven.
- **FOC frontier**: eFlexPWM + EQDC + LPADC + PWM→XBAR→ADC sync + a calibrated dq
  PMSM plant. Value-verified against first-principles goldens (phase current
  matches Ohm's law to one ADC count). The LPADC models the **A/B-side dual
  conversion** (`DualSingleEndBothSide`: A-side→RESFIFO0, B-side→RESFIFO1) the
  `mc_pmsm` demo reads Ia/Ib with; the plant drives both mux sides (`tests/
  imxrt1180-adc-ab`, mutation-proven).
- **Cortex-M7 boot + closed-loop FOC**: opt-in `boot-cm7` (auto-detected from a cm7
  ELF) boots the M7 with its own per-core view (ITCM@0x0 + DTCM@0x20000000 + SoC
  background), holding the M33. The **stock cm7 `mc_pmsm/pmsm_enc` FOC demo closes
  its control loop and spins the virtual PMSM rotor on the M7** — the FOC state
  machine reaches `kRunState_Spin` and EQDC advances under field-oriented control.
  Getting there took a chain of hardware-fidelity fixes each found by chasing the
  next blocker: DCDC/FBB/LPADC-cal unblocks + real LPADC VERID (`0x02002C1B`);
  eFlexPWM CLDOK strobe (double-buffer commit) + FSTS fault flags (W1C); QuadTimer
  CSCTRL[TCF1EN] IRQ + PCS divider + ENBL reset (the 1 ms slow loop); LPADC A/B-side
  dual conversion; and the plant's DC-bus + phase-current encoding aligned to the
  driver's Q15·12/11 convention. Peripheral IRQs route to the boot core. M33 machine
  + tests untouched (`tests/imxrt1180-cm7boot`, mutation-proven).
- **Ethernet (NETC/ENETC)**: real L2 over a QEMU socket netdev; 1180↔1180 verified
  byte-exact.
- **FlexSPI NOR**: `rom_device` XIP window + a real `m25p80`; storage-write-verified
  (erase→program→read-back byte-exact, and program-without-erase only clears bits).
- **B2B**: UART and Ethernet transports live.

The cm7 FOC loop **holds a commanded speed indefinitely under `-icount`** — driven
to 2000 rpm it settles within ~1.5% and holds flat, zero faults, `id≈0`, encoder
speed tracking the rotor to <0.5%. The old "trips FAULT_LOAD_OVER after ~0.66 rev"
was **not** a tuning gap: it was an **LPADC RESFIFO overflow**. The FOC reads a
2-command DualBoth chain in fixed `FIFO0/1/0/1 = Ia/Ib/dummy/U_DCbus` order, so
each PWM-synced trigger must leave 2+2. The ADC conversion is modelled instant, so
without a virtual clock the trigger outruns the ADC1 ISR, the FIFO overflows, the
grouping shifts, and **U_DCbus reads a phase current** (saw −15 V) → spurious
under-voltage every cycle → wedged. `-icount` rate-matches it (as conversion time
does on silicon). Model is faithful either way (drops on full + raises `STAT.FOF`).
`tools/sdk-run.sh` auto-enables `-icount` for `mc_pmsm`; invariant pinned by
`tests/imxrt1180-adc-fifo-align` (mutation-proven). **A `?:`-style "instant is fine"
peripheral timing assumption is a place a bug lives where no single-step test looks
— it only bites at rate, under the real ISR.**

The NETC switch (SW0) **NTMP command-BD ring + FDB + VLAN-filter tables +
source-MAC learning** now land — the `fsl_netc_switch` driver programs L2 tables
over the command BD ring (`CBDRPIR` doorbell → process BD → `CBDRCIR` completion);
both the `{MAC,FID}→portBitmap` FDB and the `VID→{FID,port membership}` VLAN filter
table round-trip add/query/delete; a frame ingressing the switch has its source
MAC learned into a dynamic FDB entry; the **PTP 1588 timer** (TMR0) is a
virtual-clock-derived nanosecond clock whose rate the addend tunes; and the switch
**forwards** frames per the FDB in BOTH directions (FDB∩VLAN + flood + split-horizon
egress decision: CPU→wire observed via the wire port's `PM0_TFRMN` counter, wire→CPU
proven over a QEMU mcast socket with a sentinel-barrier oracle) --
`tests/imxrt1180-netc-{fdb,ptp,fwd,rxfwd}`, all mutation-proven; the lab3 3-node
broadcast segment still passes. **The real NXP `netc_switch` SDK example runs its
control plane AND its MAC-learning data plane on the model** — EP_Init (ENETC1 mgmt
SI), 7 port-MAC resets, RTL8211F PHY link-up, `SWT_Init`/`SWT_ManagementTxRxConfig`,
and both the management + endpoint TX frame paths (on ENETC1's SI: learn src on the
egress port / forward per FDB + bump the egress port's MAC stat counters, TX-done
MSI-X via ENETC1PSI0's table) — **the whole example runs end-to-end**: learns the MAC
on each port, then forwards a frame to each port confirmed by the 512-1023-octet TX
counter. Rung-3 validation of NTMP/FDB+search/VLAN/port/management/MSI-X/statistics
against the real driver. See [[project-rt1180-netc-switch]] for the driver anchors,
the compiler-verified BD/table byte offsets, and the OCRAM DMA gotcha.

Open: the 3-node raw-L2 segment with mcxn947qemu + 95emulator (our node is
`0x88B6` on mcast `230.0.0.9:31337`); true **multi-physical-port** switch routing
(more than one wire port, needs a multi-netdev structure); the ASRC sample-rate-
converter data path (its Audio-PLL + codec blockers are now done).

## Fleet

Sibling QEMU models (`mcxn947qemu`, `93emulator`, `95emulator`, `91emulator`) share
findings over a message bus (`~/.claude/bin/bus.sh`). Several of this model's worst
bugs were found by a peer's audit and vice-versa — **when you write the model and
the test, you unconsciously test the model you wrote.**
