# i.MX RT1180 (MIMXRT1189) — peripheral coverage

Coverage of the `mimxrt1180-evk` QEMU machine.  Source of truth: the MIMXRT1189
CMSIS headers (bases/IRQs/bit masks) and the i.MX RT1180 Reference Manual
(register semantics).  **North star: fidelity-first** — a model either behaves
like silicon for arbitrary firmware, or it is register-accurate and *flags* the
gap.  Nothing fabricates a plausible-but-wrong result.

Bring-up is driven by real firmware: run a stock MCUXpresso SDK image under
`-d unimp,guest_errors`, model the first peripheral it blocks on, repeat.

## Modelled

| Block | Instances | Base(s) | Tier | Notes |
|-------|-----------|---------|------|-------|
| **Cortex-M33** (boot/secure) | 1 | — | ✅ core | ARMV7M, 239 IRQs, prio-bits 3, TrustZone-M |
| **Cortex-M7** (main) | 1 | — | ✅ core | released by the M33 (SRC/BLK_CTRL); no TZ-M |
| **LPUART** (console) | LPUART1 | 0x44380000 | ✅ functional | TX + single-entry RX + IRQ 19; same IP as i.MX 93/95; byte-access safe (eDMA) |
| **ANADIG** (OSC/PLL/PMU) | 1 | 0x44480000 | ✅ functional | OSC-stable + PLL-lock + PFD relock state machine (instant lock) |
| **CCM** (clocks) | 1 | 0x44450000 | ✅ functional | register-backed roots; LPCG STATUS0 mirrors DIRECT.ON; OBSERVE freq nominal non-zero |
| **RTWDOG** | 1..5 | 0x442D/2E0000, 0x42490/A/B0000 | ✅ functional | unlock (0xC520/0xD928) + disable; no bite modelled (flagged) |
| **S3MU** (EdgeLock ELE MU) | RT | 0x47540000 | ◐ honest handshake | TX-ready + SUCCESS reply per command; crypto results NOT faked (flagged) |
| **MU** (inter-core M33↔M7) | MU1 | 0x44220000 (MUA) / 0x44230000 (MUB) | ✅ functional | 4 TR/RR channels cross-wired + TSR/RSR flags; GCR/GSR doorbell w/ w1c handshake; per-side IRQ 21 to each NVIC |
| **SRC + BLK_CTRL_S_AONMIX** | 1 | 0x44460000 / 0x444F0000 | ✅ functional | M7 boot-vector (M7_CFG) + release (SCR.BT_RELEASE_M7), bottom-half start |
| **FlexSPI** (controller) | 1, 2 | 0x425E0000, 0x445E0000 | ◐ readiness | STS0 idle + MCR0 self-reset; no flash command engine / XIP (flagged) |
| **RGPIO** | 1..6 | 0x47400000, 0x4381/2/3/4/5 0000 | ✅ functional | PDOR/PSOR/PCOR/PTOR/PDDR/PDIR + per-pin qemu_irq out |
| **TRDC** | 1..3 | 0x44270000, 0x42460000, 0x42810000 | ◐ config stub | HWCFG0 counts + per-master DACFG.NCM (fsl_trdc DAC setup); byte-access safe; no access enforcement (flagged) |
| _everything else_ | — | 0x40000000–0x5FFFFFFF | catch-all | `unimplemented` region; `-d unimp` logs each access |

## Validated firmware

- **Stock SDK `hello_world_demo_cm33`** boots end-to-end and prints `hello world.`
  over LPUART1 (RTWDOG → ANADIG → ELE → FlexSPI → CCM → LPUART).
- **Stock SDK `led_blinky`** toggles the EVK user LED (RGPIO4[27], observable in PDOR).
- **Dual-core release** — an M33 image releases an M7 image; both print
  (`tests/imxrt1180-dualcore`).
- **Corpus harness** (`tests/imxrt1180-corpus/run.sh`) boots every prebuilt SDK
  cm33 demo and reports pass/run/fault.
- **Real Zephyr RTOS** (`mimxrt1180_evk/mimxrt1189/cm33`, secure/TZ-M) boots and
  runs multithreaded samples — `hello_world`, `synchronization` (threads +
  semaphores), `philosophers` (threads + mutexes + timers), `cpp_synchronization`
  (C++ runtime), and `condvar`.  Harness: `tests/imxrt1180-zephyr/run.sh`.
- **Zephyr ztest kernel corpus** — 15 suites, ~615 test cases, all
  `PROJECT EXECUTION SUCCESSFUL`: `common, sched/schedule_api, semaphore, queue,
  fpu_sharing/generic, mutex/mutex_api, poll, workq/work, timer/timer_api,
  mbox/mbox_api, fifo/fifo_api, lifo/lifo_api, stack/stack, mem_slab/mslab_api,
  sleep`.  Covers scheduler, sync primitives, timers, work queues, IPC, memory
  management, and FPU context sharing.  (Secure `CONFIG_ASSERT=y` builds exercise
  the TRDC DACFG model.)
- **FPU** validated by `fpu_sharing/generic` — FP load/store save/restore across
  context switches (lazy stacking) + a 13 s π computation, both pass.

## Known gaps (surfaced by the demo corpus)

| Needed for | Block | Base | Status |
|-----------|-------|------|--------|
| bubble_peripheral | **LPI2C** (accelerometer) | 0x44350000 (LPI2C2) | not modelled |
| sai | **SAI + eDMA** audio path | — | past the TRDC assert; data path not modelled |
| usb_device_dfu | **USB OTG + PHY** | 0x42C80000 | not modelled → guest faults |
| motor-control frontier | **eFlexPWM + QDC encoder + ADC-sync** | — | the headline RT1180 feature; not yet modelled |

## Roadmap

Model LPI2C, the inter-core MU (dual-core messaging), then SAI/eDMA and USB, to
clear the demo corpus; then the motor-control block (the RT1180's distinguishing
silicon — eFlexPWM + quadrature encoder + ADC↔PWM sync), and a virtual-motor
plant so a FOC control loop can close in emulation.
