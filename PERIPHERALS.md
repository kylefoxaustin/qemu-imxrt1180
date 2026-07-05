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
| **LPI2C** (controller mode) | 1..4 | 0x44340000, 0x44350000, 0x42530000, 0x42540000 | ✅ functional | command-FIFO master (START/TX/RX/STOP) on a real QEMU I2CBus — devices attach; MSR flags + NDF NACK detect + IRQ (13/14/62/63) |
| **LPSPI** (controller mode) | 1..4 | 0x44360000, 0x44370000, 0x42550000, 0x42560000 | ✅ functional | full-duplex SPI master (TCR frame/PCS/CONT, TDR→SSIBus→RDR) with per-CS lines; validated vs a serial-flash JEDEC-ID read; IRQ (16/17/65/66) |
| **LPIT** (periodic timer) | 1..3 | 0x442F0000, 0x424C0000, 0x42CC0000 | ✅ functional | 4-channel ptimer-backed 32-bit periodic down-counter; TVAL/CVAL + MSR.TIF W1C + MIER IRQ (15/64/149); validated (periodic IRQ + counter) |
| **FlexCAN** (CAN/CAN-FD) | 1..3 | 0x443A0000, 0x425B0000, 0x445B0000 | ✅ functional | MCR freeze/disable/soft-reset handshakes; 96 message buffers (TX/RX CODE); real frames on a QEMU can-bus + internal loopback; IFLAG/IMASK IRQ (8/51/191); adapted from the MCX FlexCAN |
| **eDMA** (enhanced DMA) | eDMA3 (32ch), eDMA4 (64ch) | 0x44000000, 0x42000000 | ✅ functional | TCD-driven mem-to-mem (SADDR/DADDR/SOFF/DOFF/ATTR/NBYTES/CITER); START triggers a real address_space transfer + DONE + INTMAJOR; per-ch IRQ (eDMA3 95+, eDMA4 grouped 128+); adapted from the MCX eDMA |
| **SAI** (I2S audio) | 1..4 | 0x443B0000, 0x42BB0000, 0x42BC0000, 0x42BD0000 | ◐ bring-up | TCSR/RCSR SR+FR resets self-clear, TX FIFO advertises space (FWF), RX empty; init handshake settles; no audio streaming (flagged); IRQ 45/198/199/154; adapted from the MCX SAI |
| **SRC + BLK_CTRL_S_AONMIX** | 1 | 0x44460000 / 0x444F0000 | ✅ functional | M7 boot-vector (M7_CFG) + release (SCR.BT_RELEASE_M7), bottom-half start |
| **FlexSPI** (controller) | 1, 2 | 0x425E0000, 0x445E0000 | ◐ readiness | STS0 idle + MCR0 self-reset; no flash command engine / XIP (flagged) |
| **RGPIO** | 1..6 | 0x47400000, 0x4381/2/3/4/5 0000 | ✅ functional | PDOR/PSOR/PCOR/PTOR/PDDR/PDIR + per-pin qemu_irq out |
| **TRDC** | 1..3 | 0x44270000, 0x42460000, 0x42810000 | ◐ config stub | HWCFG0 counts + per-master DACFG.NCM (fsl_trdc DAC setup); byte-access safe; no access enforcement (flagged) |
| **USBPHY** | 1..2 | 0x42CA0000, 0x42CB0000 | ✅ functional | USB-HS PHY PLL: RW/SET/CLR/TOG register bank; PLL_SIC.PLL_LOCK reported once powered (instant lock, like ANADIG); no UTMI/charger-detect |
| **USB OTG** (device) | 1..2 | 0x42C80000, 0x42C90000 | ◐ device bring-up | ChipIdea USB-HS: CAPLENGTH/DCIVERSION/DCCPARAMS (device+host capable, 8 EP), USBCMD.RST self-clear, USBSTS/ENDPT* W1C; init+run complete. No host attached → no enumeration/transfers (flagged, not faked); IRQ 215/214 |
| **eFlexPWM** | 1..4 | 0x42650000, 0x42660000, 0x42670000, 0x42680000 | ◐ functional | Motor-control PWM. 4 submodules/module; INIT+VAL0..5 double-buffered (commit on MCTRL.LDOK); MCTRL.RUN starts a ptimer-backed reload at modulo/(clk/prescaler); STS.RF + INTEN.RIE raise the submodule reload IRQ (the FOC loop clock); PWMA duty computed (per-mille). SM0..3 IRQ 24-27/171-174/176-179/181-184 + fault 23/170/175/180. No motor plant/encoder responds + ADC-sync XBAR routing not modelled (flagged) |
| **EQDC** (encoder) | 1..4 | 0x42710000, 0x42720000, 0x42730000, 0x42740000 | ◐ functional | Quadrature decoder. CTRL.LDOK self-clearing load (SWIP preloads UPOS:LPOS from UINIT:LINIT); coherent 32-bit read (UPOS read snapshots LPOS/REV/POSD → hold registers); register-accurate position/rev/diff counters. IRQ 185-188. No encoder/plant drives the inputs → counters do not advance on their own (flagged) |
| **LPADC** | 1..2 | 0x42600000, 0x42E00000 | ◐ functional | 12/16-bit SAR ADC. CTRL soft-reset/FIFO-reset self-clear; CAL_REQ reports calibration done (STAT.CAL_RDY, GCR.RDY); SWTRIG + 8 HW-trigger inputs run the TCTRL/CMD command chain (CMDH.NEXT), pushing tagged results (VALID/TSRC/loop) into RESFIFO; FCTRL.FCOUNT + STAT.RDY + IE watermark IRQ (93/189). No analog front-end/plant → each result is a fixed mid-scale placeholder (flagged, not a fabricated current) |
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
- **Stock SDK `usb_device_dfu` (bm / freertos / lite)** boot the USB HID-mouse
  device stack to its `USB device HID mouse demo` banner: PHY PLL locks, the
  ChipIdea controller initialises and runs, then idles waiting for a host (none
  attached — the honest end-state).  Focused model test: `tests/imxrt1180-usb`
  (`USB: PASS` — PHY PLL lock + controller reset/DCCPARAMS/run).  Note: raw SDK
  `.bin` load base is derived from the image's own reset vector (the DFU images
  link at 0x0FFF0000, reserving the low 64 KiB of CODE_TCM as the update slot).
- **eFlexPWM** (`tests/imxrt1180-pwm`, `PWM: PASS`) — a center-aligned 3-phase
  setup (as in the FOC `mc_periph_init`): INIT/VAL double-buffering commits on
  LDOK, the submodule reload IRQ fires periodically (the FOC control-loop clock),
  and the counter advances.  Register map from the MIMXRT1189 DFP `PERI_PWM.h`.
- **EQDC** (`tests/imxrt1180-eqdc`, `EQDC: PASS`) — CTRL.LDOK self-clears, SWIP
  preloads the position from UINIT:LINIT, and the coherent-read latch snapshots
  LPOS/REV/POSD into the hold registers on a UPOS read.  Map from `PERI_EQDC.h`.
- **LPADC** (`tests/imxrt1180-adc`, `ADC: PASS`) — CAL_REQ reports calibration
  complete, a software trigger runs a command chain (cmd1→cmd2 via CMDH.NEXT),
  and the tagged results are read back from the FIFO.  Map from `PERI_ADC.h`.

## Known gaps (surfaced by the demo corpus)

| Needed for | Block | Base | Status |
|-----------|-------|------|--------|
| sai | **SAI + eDMA** audio path | — | past the TRDC assert; data path not modelled |
| usb_device_dfu | **USB host enumeration** | 0x42C80000 | controller inits + runs; no host attached, so the device does not enumerate (bridging to QEMU's USB host framework is future work) |
| motor-control frontier | **PWM→ADC sync wiring + virtual plant** | — | eFlexPWM + EQDC + LPADC done (ADC has HW-trigger inputs ready); still need the eFlexPWM trigger output + XBAR routing to drive them, and a virtual-motor plant (duty → speed → EQDC count → ADC current) to close a FOC loop |

## Roadmap

The motor-control frontier is underway: **eFlexPWM** (PWM1-4, double-buffered
compare registers + the periodic reload interrupt a FOC loop runs on) and the
**EQDC** quadrature encoder (position/rev counters + coherent read) and the
**LPADC** (command-chain conversion + result FIFO, with hardware trigger inputs)
are modelled.  Next on that track: give the eFlexPWM a trigger output and route
it through the **XBAR** to the ADC's trigger inputs (the actual PWM→ADC
synchronised sampling), then a small virtual-motor plant (duty → velocity → EQDC
count → ADC current) so a FOC control loop can actually close in emulation — the
distinguishing capability none of the fleet has.

Remaining demo-corpus items are reference-blocked (fidelity-first): the SAI
`sai.c:467` assert semantics have no source in the cache, and the
`bubble_peripheral` FXLS8974 sensor driver (register map) is absent.  Optionally,
bridge the USB device controller to QEMU's USB host framework for real
enumeration.
