# qemu-imxrt1180

![i.MX RT1180 — dual-core Cortex-M7 + Cortex-M33 crossover MCU](rt1180_hero.png)

A QEMU machine model of the **NXP i.MX RT1180** crossover MCU (the fully-loaded
**MIMXRT1189** on the MIMXRT1180-EVK): a heterogeneous dual-core part pairing a
secure **Cortex-M33** boot core with a **Cortex-M7** application core, targeting
real-time **motor control** (eFlexPWM + quadrature encoder) and **Gb TSN**
industrial networking.

- **Fork of** QEMU mainline (work on the `imxrt1180-dev` branch); all model
  logic in self-contained `imxrt1180_*` files — long-term aim: upstream-mergeable.
- **What sets it apart vs. its fleet siblings** (i.MX 91/93/95, MCXN947): the
  only crossover MCU with the M7+M33 pair and the motor-control + TSN frontier.
- **Maintainer:** @kylefoxaustin.
- **North star:** fidelity-first — a silently-wrong answer is the worst bug; a
  model is register-accurate and *flags* any gap rather than faking a result.

## Quickstart

```sh
./configure --target-list=arm-softmmu
make -j"$(nproc)"

# Boot the stock MCUXpresso SDK hello_world over the LPUART1 console:
./build/qemu-system-arm -M mimxrt1180-evk -display none -monitor none \
    -kernel hello_world_demo_cm33.bin \
    -serial stdio -semihosting-config enable=on,target=native
# -> hello world.
```

## What runs today

| Subsystem | Tier | Evidence |
|-----------|------|----------|
| Cortex-M33 boot + memory map + NVIC | ✅ | boots bare-metal + stock SDK firmware |
| LPUART1 console | ✅ | stock `hello_world` prints `hello world.` |
| Clocks (ANADIG PLL/PFD, CCM roots/gates/observe) | ✅ | real SDK `CLOCK_Init` / `GetFreqFromObs` complete |
| RTWDOG, TRDC, FlexSPI, EdgeLock ELE MU | ✅/◐ | SDK SystemInit runs to the console |
| RGPIO | ✅ | stock `led_blinky` toggles the user LED (RGPIO4[27]) |
| **Dual-core: M33 releases the Cortex-M7** | ✅ | `tests/imxrt1180-dualcore` — both cores print |

See [PERIPHERALS.md](PERIPHERALS.md) for the full coverage table and gaps.

## Interconnect (board-to-board) ✅

A live b2b node on two transports:

- **Ethernet** — the NETC (ENETC endpoint) is a real QEMU NIC. 1180↔1180 verified
  byte-exact, and the RT1180 is node `0x88B6` of a **three-SoC cross-silicon L2
  segment** (see below). `tools/netc-eth-{b2b,lab3}.sh`.
- **UART** — LPUART2 @ `0x44390000` on a socket chardev (`serial_hd(1)`), matching
  the fleet's `uart-link-imx-mcx` cell, so RT1180↔MCX/91 is turnkey.
  `tests/imxrt1180-uartlink`.

Still to crib from the fleet: CAN (`can-host-chardev`), SPI (`spi-link`),
I2C (`i2c-link`) — the controllers are modelled; only the bridge wiring is left.

## Validation

- `hello_world_demo_cm33.bin` → `hello world.` over LPUART1 (full board init).
- `led_blinky` → RGPIO4[27] toggles (observable in PDOR).
- `tests/imxrt1180-dualcore` → M33 releases M7; both cores print.
- `tests/imxrt1180-corpus/run.sh` → boots every prebuilt SDK cm33 demo, reports
  pass / run / fault.

## Required artifacts

This is an MCU, not a Linux applications processor — so instead of a kernel /
DTB / rootfs, the "artifact" is **firmware**: a bare-metal, Zephyr, or
MCUXpresso image (`-kernel <elf|bin>`).  The SDK's prebuilt `cm33/*.bin` demos
(in the EVK SDK zip) work directly; RAM/debug images link their vector table to
the code TCM (`0x0FFE0000`).  _(Linux/DTB/rootfs rows: N/A — no Linux on this MCU.)_

## Building

`./configure --target-list=arm-softmmu && make -j"$(nproc)"`, then
`./build/qemu-system-arm -M mimxrt1180-evk ...`.  Requires the usual QEMU build
deps; the bare-metal tests use `arm-none-eabi-gcc`.

## Architecture

`hw/arm/imxrt1180_soc.c` builds the SoC (dual ARMV7M cores, memory map, catch-all
peripheral window, then the modelled peripherals); `hw/arm/imxrt1180_evk.c` is
the thin board.  Each peripheral is a self-contained `imxrt1180_*` device under
`hw/{char,misc,gpio}/`.  The M7 is released from a bottom-half on
`SRC_GENERAL.SCR.BT_RELEASE_M7`, booting from `BLK_CTRL_S_AONMIX.M7_CFG.INITVTOR`.

## Repository tour

- `hw/arm/imxrt1180_{soc,evk}.c` — SoC + board
- `hw/char/imxrt1180_lpuart.c` — console UART
- `hw/misc/imxrt1180_{anadig,ccm,rtwdog,s3mu,flexspi,src,trdc}.c` — clocks, watchdog, ELE MU, FlexSPI, M7-release, TRDC
- `hw/gpio/imxrt1180_rgpio.c` — GPIO
- `tests/imxrt1180-{hello,dualcore,corpus}/` — bring-up + dual-core + saturation tests
- `include/hw/{arm,char,misc,gpio}/imxrt1180_*.h` — headers

## Cross-silicon: three SoCs on one wire ✅

`tools/netc-eth-lab3.sh` — the RT1180 is node **`0x88B6`** of a three-node raw-L2
segment with [`mcxn947qemu`](https://github.com/kylefoxaustin/mcxn947qemu) and the
i.MX 95 model. **Verified live, 2026-07-12:**

```
ENET-LAB3 rx: peer ethertype 0x88b5 src 02:4d:43:58:00:01   <- MCXN947 (Cortex-M33, ENET-QoS)
ENET-LAB3 rx: peer ethertype 0x88b7 src 02:49:4d:58:95:01   <- i.MX 95 (Cortex-A55, Linux, ENETC)
ENET-LAB3 PASS: saw BOTH peers on the segment
```

Three QEMU machine models, three different Ethernet MAC IPs (ENET-QoS / NETC /
ENETC), two architectures, two bare-metal Cortex-M33s and a Linux Cortex-A55 —
each running its own vendor firmware, exchanging real L2 frames. Each node must
observe **both** others by EtherType before it passes; a node swimming in traffic
from one peer **refuses** the milestone. That assertion declined a green four
times before it earned one.

Building the node also found a NETC bug **no two-node test can reach**: a QEMU
`can_receive()` returning false stalls the RX queue *permanently* unless the
device calls `qemu_flush_queued_packets()`. Two nodes boot together, so the window
never opens — it opens only for a node joining traffic **already in flight**.

## How this model is validated

A model that *runs* is not a model that is *right*. Two rules, both learned the
hard way (see `CLAUDE.md`):

- **Tests must compare a value to an independently-derived expected value.**
  "An IRQ fired", "a VALID bit was set", "it's within a range" proves nothing —
  **a range is not a golden**. The phase current is checked against Ohm's law
  (matches to *one ADC count*); the PWM period against SysTick, an Arm core timer
  outside the model; NOR programming against real erase/program physics.
- **A test that cannot fail is decoration, and you cannot tell by reading it.**
  `tools/mutation-audit.sh` corrupts the model on purpose and requires each test
  to notice. When it was first run, **3 of 4 tests did not** — the whole FOC path
  was blind. Goldens are also **swept across shapes**, because a model can be
  right at one prescaler and wrong at another.

## Known limitations

Honest gaps, per-block, are in [PERIPHERALS.md](PERIPHERALS.md); `(flagged)` is
defined there and means *visible to the guest*, never "we wrote a host log".

- **Clocks** report nominal, not computed, frequencies — **and this is a live
  fidelity gap, not just a stub.** The stock NXP FOC demo (`mc_pmsm`) targets a
  16 kHz carrier and derives its PWM registers from the *real* clock root via
  `CLOCK_GetRootClockFreq()`; its whole control-loop timestep depends on that
  being true. Because our CCM does not compute root frequencies, the **absolute
  emulated PWM carrier frequency is unverified** — our value-golden verifies that
  the PWM honours `VAL1`/`INIT`/`PRSC` (swept across both axes), but it takes the
  *clock* from the model, so it cannot catch a wrong clock. **A test cannot
  validate its own trust anchor.** Closing this means making CCM compute the root
  frequency from the PLL config firmware programs, so the golden can be anchored
  on the firmware's own intent instead of on a constant we chose.
- **TRDC** does not enforce access control (grants everything).
- **EdgeLock (ELE)**: the enclave is proprietary and not modelled. Its **RNG is
  real** (genuine `qemu_guest_getrandom` entropy DMA'd to the guest). **Every
  other crypto command returns a failure status to the guest and leaves the
  result buffer untouched** — it does not compute, and it says so.
  > ⚠️ This README previously claimed the enclave *"never fabricates a crypto
  > result"*. **That was false.** The model answered *every* ELE command with
  > `RESPONSE_SUCCESS`, so `ELE_RngGetRandom()` returned `kStatus_Success` over an
  > un-written buffer — firmware would have seeded a crypto stack with un-computed
  > data and believed it succeeded. Fixed 2026-07-12. The false claim is left
  > visible rather than quietly deleted.
- **LPADC A/B input side** (`CMDL.SIDE`) is not modelled, which is what currently
  blocks running NXP's stock `mc_pmsm` FOC demo unmodified.
- **No audio subsystem** (ASRC / audio PLL / codec); **NETC L2 switch path** is
  not modelled (the ENETC *endpoint* is).
- **Cache** is a QEMU-architectural WONTFIX (no guest CPU cache to model); **MECC**
  is an optional RAS diagnostic.

## Roadmap

1. **LPADC A/B side mux** → run the stock `mc_pmsm` FOC demo unmodified.
2. **NETC switch path** (SW0/FDB), multi-SI, PTP 1588.
3. Saturation/thermal effects and a time-varying load profile in the motor plant.
4. Value-golden a peripheral **through the real `fsl_*` driver** rather than by
   poking registers — the one rung-3 clause we do not yet satisfy.

## License

GPL-2.0-or-later (QEMU).  Upstream QEMU documentation: see `README.rst`.
