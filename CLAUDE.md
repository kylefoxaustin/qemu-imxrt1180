# CLAUDE.md — MCX N947 QEMU machine model (build agent: mcxqemu)

## Mission

Integrate, build, and bring up a QEMU machine model for the **NXP MCXN947**
(dual Arm Cortex-M33) into a local QEMU checkout, then iterate peripheral
support. Target machine: `frdm-mcxn947`. Keep the code clean, self-contained,
and in-tree-style — all model logic in the `mcxn_*` files, no edits to generic
QEMU. This is a private model in `kylefoxaustin/mcxn947qemu`; nothing here is
submitted anywhere.

The human (Kyle) maintains the i.MX 93/95/91 QEMU models. Reuse that experience:
the patterns here mirror the i.MX work, and the console UART is the **same
LPUART IP** as i.MX 93/95.

## Source of truth

- Device facts (IRQ count, prio bits, peripheral bases, bit masks) come from the
  MCXN947 CMSIS header. Do **not** invent addresses — pull them from:
  `https://raw.githubusercontent.com/nxp-mcuxpresso/mcux-sdk/main/devices/MCXN947/MCXN947_cm33_core0.h`
  (or the local MCUXpresso SDK copy on this machine).
- Code is written against **current QEMU mainline** idioms. If the local tree is
  older, see "Version touchpoints" below.

## Files in this drop

```
include/hw/arm/mcxn_soc.h       SoC type + per-SKU MCXNConfig table struct
include/hw/char/mcxn_lpuart.h   LPUART/FlexComm console device
include/hw/misc/mcxn_scg.h      SCG clock-generator stub
hw/arm/mcxn_soc.c               config table, memories, M33 core, periph stub, console+SCG wiring
hw/arm/mcxn_frdm.c              frdm-mcxn947 board
hw/char/mcxn_lpuart.c           LPUART console model (CMSIS-exact bit semantics)
hw/misc/mcxn_scg.c              SCG clock-generator stub (reports clocks ready)
QEMU-INTEGRATION.md             exact Kconfig / meson.build edits + file placement
README.md                       design notes, memory map, verified facts
```

## Build steps

1. Copy the five source files into the QEMU tree per `QEMU-INTEGRATION.md`.
2. Apply the `Kconfig` and `meson.build` additions from the same doc
   (`hw/arm/Kconfig`, `hw/char/Kconfig`, `hw/arm/meson.build`, `hw/char/meson.build`).
3. Configure and build only the arm-softmmu target:
   ```sh
   ./configure --target-list=arm-softmmu
   make -j"$(nproc)"
   ```
4. Confirm the machine registered:
   ```sh
   ./build/qemu-system-arm -M help | grep frdm-mcxn947
   ```

## Smoke test (first milestone)

No firmware needed to prove the machine builds and the core boots. With a tiny
bare-metal or Zephyr `hello_world` ELF:

```sh
./build/qemu-system-arm \
    -M frdm-mcxn947 \
    -kernel hello.elf \
    -nographic \
    -serial mon:stdio \
    -semihosting-config enable=on,target=native \
    -d unimp,guest_errors
```

Two valid "it works" outcomes:
- Output via **semihosting** (works before any UART): proves CPU + memory + boot.
- Output via the **FlexComm4 console** (`-serial mon:stdio`): proves the LPUART
  model. The Zephyr `frdm_mcxn947` console is FlexComm4 — a Zephyr `hello_world`
  built for `frdm_mcxn947/mcxn947/cpu0` is the ideal test image.

## Bring-up iteration loop

The peripheral window is a catch-all `unimplemented` region. Every access into
unmodelled space is logged by `-d unimp,guest_errors`. That log is the
prioritised to-do list:

1. Run firmware, capture the unimp log.
2. Identify the first peripheral the firmware blocks on.
3. Implement it as a `SysBusDevice`, map it (it overrides the catch-all by
   memory-region priority), connect its IRQ via
   `qdev_get_gpio_in(armv7m, <IRQn>)`.
4. Rebuild, rerun, repeat.

Pull register layouts and IRQ numbers from the CMSIS header, exactly as the
LPUART model was built.

## Verified MCXN947 facts

| Property           | Value                                             |
|--------------------|---------------------------------------------------|
| Core               | dual Cortex-M33 @ 150 MHz (MVP wires cpu0 only)    |
| Core features      | FPU, DSP, MPU, SAU/TrustZone-M                     |
| NVIC external IRQs | 156 (highest CTI0_IRQn = 155)                      |
| __NVIC_PRIO_BITS   | 3                                                 |
| Flash              | 2 MiB @ 0x0000_0000                               |
| SRAM               | 512 KiB @ 0x2000_0000                             |
| Console            | FlexComm4 / LPUART4 @ 0x400B_4000, IRQ 39          |
| FlexCAN            | CAN0 @ 0x400D_4000, CAN1 @ 0x400D_8000            |
| Neutron NPU        | IRQ 97; base from RM (not in CMSIS header)         |

TrustZone-M: peripherals are aliased non-secure @ `0x400x_xxxx` and secure @
`0x500x_xxxx`. The catch-all spans `0x4000_0000..0x5FFF_FFFF` to cover both.

## Version touchpoints (check first if the build fails)

Written for current mainline. On an older QEMU tree, adjust:

1. **`serial_hd` include** — mainline: `system/system.h`; older:
   `sysemu/sysemu.h`. (in `hw/arm/mcxn_soc.c`)
2. **`device_class_set_legacy_reset`** — newer API. Older trees:
   `dc->reset = mcxn_lpuart_reset;`. (in `hw/char/mcxn_lpuart.c`)
3. **Property arrays** — mainline dropped the `DEFINE_PROP_END_OF_LIST()`
   terminator. If the compiler complains, add it back to the two `Property[]`
   arrays.
4. **`armv7m_load_kernel(cpu, name, mem_base, mem_size)`** — the `mem_base`
   (3rd) arg is relatively recent. Older signature drops it. (in `hw/arm/mcxn_frdm.c`)
5. **`ARMV7M` clock inputs** are `cpuclk` / `refclk` on mainline (confirmed).

## Status / next work (priority order)

- [x] SoC scaffold: M33 core, flash/SRAM, catch-all peripheral stub.
- [x] `frdm-mcxn947` board, clocks, kernel load.
- [x] FlexComm4 / LPUART4 console (TX + RX + IRQ), NS + secure alias.
- [x] SCG0 clock-generator stub (oscillators/PLLs report ready), NS + secure alias.
- [ ] **Build + smoke test** ← start here.
- [ ] PORT/GPIO0..5 (pin mux + basic GPIO).
- [ ] cpu1 (second M33) — see README; mirrors the Zephyr dual-core enable.
- [ ] FlexCAN (CAN0/1) — likely portable from the i.MX FlexCAN model.
- [ ] eIQ Neutron NPU — behavioural SysBusDevice; reuse the i.MX 95 / ZV3400
      approach; needs the NPU base from the RM.

## Guardrails

- Never fabricate register offsets, base addresses, or IRQ numbers — derive them
  from the CMSIS header or the RM. A wrong offset = a silent firmware hang.

- **Never report SUCCESS for something you did not compute. TELL THE GUEST.**
  Declining to model a block is fine; a model may fail. What it may not do is
  tell the firmware it succeeded at something it never did.
  1. **Where does the result land?** If it lands in *a pointer the guest gave
     you*, then "acked + buffer untouched" is a silent-wrong that no reply-shape
     heuristic and no IRQ-counting test will ever see. (This is exactly how the
     ELE `GET_RNG_RANDOM` bug hid: the reply carries no data, so nothing looked.)
  2. **Who gets told?** A `LOG_UNIMP`/QMP flag reaches the **operator**. The
     firmware under test *cannot see it*. Honest-to-the-host while lying-to-the-
     guest is lying. **An out-of-band flag is not an honest fault.**
  3. **Fault through the block's own documented, NON-GATING error channel** — a
     status/error field, an error-trap IRQ. Never by withholding the completion:
     that *hangs* the driver instead of *informing* it.
  4. **Compute it if you can.** Decline only what you genuinely cannot produce.
     Entropy, for instance, you *can* produce — so `GET_RNG_RANDOM` returns real
     `qemu_guest_getrandom` bytes rather than an honest refusal.
  5. Any escape hatch that restores a fabricated success is a **property,
     default OFF** (e.g. `imxrt1180-s3mu.fake-uncomputed-success`).

- **A test that cannot fail is decoration, and you cannot tell by reading it.**
  Break the model on purpose and demand the test notice — `tools/mutation-audit.sh`.
  Guarded tests read a value back and compare it to an **independently-derived
  expected value**. Anything that stops at "a flag set", "an IRQ fired", or "it's
  within a range" proves nothing: a range hides a 3×-wrong value *and* an
  unconverged one. When you add a time budget or a negative test, **prove it can
  fail** — an unmeasured threshold is a decoration, and a negative test can rot
  green when the model improves underneath it.
- Keep cpu0-only until single-core boot is solid.
- Report the unimp log back after the first firmware run so the peripheral order
  is driven by real firmware behaviour, not guesswork.
