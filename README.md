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

## Interconnect (board-to-board)

Planned to join the fleet's holobench lab as a b2b node over stock chardev
sockets (UART/SPI/CAN/I2C/USB/ENET), reusing the shared `spi-link` /
`can-host-chardev` / `i2c-link` bridges — not yet wired. _(N/A today.)_

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

## Known limitations

LPI2C, the inter-core MU (MU1), SAI/eDMA data paths, and USB OTG are not yet
modelled (their demos hang or fault — see PERIPHERALS.md).  The headline
**motor-control** block (eFlexPWM + QDC encoder + ADC↔PWM sync) and a
virtual-motor plant are the next frontier.  Clocks report nominal (not
computed) frequencies; TRDC does not enforce access control; the EdgeLock
enclave completes the handshake but never fabricates a crypto result.

## Roadmap

1. Clear the demo corpus — LPI2C, MU1 inter-core, SAI/eDMA, USB.
2. Board-to-board holobench node (UART/CAN first).
3. **Motor control** — eFlexPWM + quadrature encoder + ADC-sync, then a
   settable virtual-motor plant so a FOC loop closes in emulation (the fleet's
   first real closed control loop).

## License

GPL-2.0-or-later (QEMU).  Upstream QEMU documentation: see `README.rst`.
