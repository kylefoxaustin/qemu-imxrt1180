# i.MX RT1180 (MIMXRT1189) — peripheral coverage

Coverage of the `mimxrt1180-evk` QEMU machine.  Source of truth: the MIMXRT1189
CMSIS headers (bases/IRQs/bit masks) and the i.MX RT1180 Reference Manual
(register semantics).  **North star: fidelity-first** — a model either behaves
like silicon for arbitrary firmware, or it is register-accurate and *flags* the
gap.  Nothing fabricates a plausible-but-wrong result.

Bring-up is driven by real firmware: run a stock MCUXpresso SDK image under
`-d unimp,guest_errors`, model the first peripheral it blocks on, repeat.

## Modelled

### What "(flagged)" means — read this before you trust a row

`(flagged)` appears on ~20 rows below and, until 2026-07-12, **was never defined
anywhere**. A load-bearing policy word with no definition is not a safeguard: it
is a licence, because it forbids nothing. (mcxn947qemu traced *four* silent-wrong
blocks in their tree to one undefined sentence in exactly this position — *"your
code is not the bug, your policy is, and it will regenerate the bug after you fix
it."*) So, explicitly:

> **`(flagged)` means: this gap is visible TO THE GUEST, or the guest cannot act
> on it.** It does **not** mean "we wrote a `LOG_UNIMP`". A host-side log reaches
> the *operator*; the firmware under test cannot see it. Being honest to the host
> while telling the guest an operation succeeded **is lying**, and it is the
> single failure mode that has produced the worst bugs in this tree.
>
> A flagged gap must be one of:
> - **absent** — the block never claims the thing happened (USB never asserts
>   enumeration or an endpoint completion; CMP never raises an edge). The guest
>   sees a device with nothing attached, which is the truth.
> - **inert but honest** — an un-driven input reads its documented un-energised
>   value (LPADC returns mid-scale for a channel no plant drives). It is a
>   *specific documented value*, and a test asserts that value.
> - **an explicit failure to the guest**, through the block's own documented
>   **non-gating** error channel (S3MU/ELE returns a non-success status for a
>   crypto result it did not compute). Never by withholding the completion —
>   that hangs the driver instead of informing it.
>
> What `(flagged)` may **never** mean: the block acked, the guest's result buffer
> was left untouched or stale, and the truth went to a log the firmware cannot
> read. **If the result travels by a pointer the guest gave you, an ack plus an
> untouched buffer is a silent-wrong that no status check and no IRQ-counting
> test will ever see.**

| Block | Instances | Base(s) | Tier | Notes |
|-------|-----------|---------|------|-------|
| **Cortex-M33** (boot/secure) | 1 | — | ✅ core | ARMV7M, 239 IRQs, prio-bits 3, TrustZone-M |
| **Cortex-M7** (main) | 1 | — | ✅ core | released by the M33 (SRC/BLK_CTRL); no TZ-M |
| **LPUART** | LPUART1, LPUART2 | 0x44380000, 0x44390000 | ✅ functional | TX + single-entry RX + IRQ 19/20; same IP as i.MX 93/95; byte-access safe (eDMA). LPUART1 = console (serial_hd(0)); LPUART2 = board-to-board link port (serial_hd(1), a socket chardev to a peer) |
| **ANADIG** (OSC/PLL/PMU) | 1 | 0x44480000 | ✅ functional | OSC-stable + PLL-lock + PFD relock state machine (instant lock) |
| **CCM** (clocks) | 1 | 0x44450000 | ✅ functional | **real clock tree**: `root_hz = source(CLOCK_ROOT[n].MUX) / (DIV+1)`, PLL/OSC frequencies computed from ANADIG as `CLOCK_GetPllFreq()` does; the 74-root mux table is taken mechanically from `fsl_clock.c`. All six timer blocks read it at the point of use. LPCG STATUS0 mirrors DIRECT.ON. OBSERVE.FREQUENCY_CURRENT is **computed** (it used to return a fabricated 6 MHz into guest arithmetic). Reset values are the RM's cold-POR column. Gate: `tests/imxrt1180-clocktree` |
| **RTWDOG** | 1..5 | 0x442D/2E0000, 0x42490/A/B0000 | ✅ functional | unlock (0xC520/0xD928) + disable; no bite modelled (flagged) |
| **S3MU** (EdgeLock ELE MU) | RT | 0x47540000 | ◐ honest enclave | 8 TR / 4 RR (CMSIS `S3MU_TR_COUNT`=8). **RNG is REAL** — `GET_RNG_RANDOM`/`START_RNG` DMA genuine `qemu_guest_getrandom` entropy into the guest's buffer (unpredictable; reproducible only under `-seed`). Coordination commands (CLOCK/VOLTAGE_CHANGE, RELEASE_RDC, PING) answer SUCCESS truthfully; `GET_FW_STATUS` returns 0 = *"no ELE FW in place"*, which is true here. **Every other command — all crypto — returns a NON-SUCCESS status to the guest and leaves its result buffer untouched.** Escape hatch `fake-uncomputed-success` (default OFF). ⚠️ **This row previously read "crypto results NOT faked" — that was FALSE**: the model answered *every* command with `RESPONSE_SUCCESS`, so `ELE_RngGetRandom()` returned `kStatus_Success` over an un-written buffer and firmware would have seeded a crypto stack with un-computed data. Fixed 2026-07-12; the false claim is left visible rather than quietly deleted |
| **MU** (inter-core M33↔M7) | MU1 | 0x44220000 (MUA) / 0x44230000 (MUB) | ✅ functional | 4 TR/RR channels cross-wired + TSR/RSR flags; GCR/GSR doorbell w/ w1c handshake; per-side IRQ 21 to each NVIC |
| **LPI2C** (controller mode) | 1..4 | 0x44340000, 0x44350000, 0x42530000, 0x42540000 | ✅ functional | command-FIFO master (START/TX/RX/STOP) on a real QEMU I2CBus — devices attach; MSR flags + NDF NACK detect + IRQ (13/14/62/63) |
| **LPSPI** (controller mode) | 1..4 | 0x44360000, 0x44370000, 0x42550000, 0x42560000 | ✅ functional | full-duplex SPI master (TCR frame/PCS/CONT, TDR→SSIBus→RDR) with per-CS lines; validated vs a serial-flash JEDEC-ID read; IRQ (16/17/65/66) |
| **LPIT** (periodic timer) | 1..3 | 0x442F0000, 0x424C0000, 0x42CC0000 | ✅ functional | 4-channel ptimer-backed 32-bit periodic down-counter; TVAL/CVAL + MSR.TIF W1C + MIER IRQ (15/64/149); validated (periodic IRQ + counter) |
| **FlexCAN** (CAN/CAN-FD) | 1..3 | 0x443A0000, 0x425B0000, 0x445B0000 | ✅ functional | MCR freeze/disable/soft-reset handshakes; 96 message buffers (TX/RX CODE); real frames on a QEMU can-bus + internal loopback; IFLAG/IMASK IRQ (8/51/191); adapted from the MCX FlexCAN |
| **eDMA** (enhanced DMA) | eDMA3 (32ch), eDMA4 (64ch) | 0x44000000, 0x42000000 | ✅ functional | **Verified against the real `fsl_edma` driver: 9 of 9 stock NXP eDMA `driver_examples` pass, data-checked** (`memory_to_memory`, `..._transfer`, `memset`, `channel_link`, `wrap_transfer`, `interleave_transfer`, `ping_pong_transfer`, `scatter_gather`, and eDMA3's `memory_to_memory`). **Channel geometry from the CMSIS header**: DMA3 `CH[n]` @ `+0x10000 + n*0x10000` (`PERI_DMA.h`), DMA4 `TCD[n]` @ `+0x10000 + n*0x8000` (`PERI_DMA4.h`). **A service request moves ONE MINOR LOOP (NBYTES)** and decrements CITER — and `TCD_CSR[START]`, a peripheral request line, and a channel link are all the SAME event (RM 5.4: software START "follows the same basic flow as peripheral requests"). START is auto-cleared on execution. **Peripheral requests**: `CH_CSR[ERQ]` + `CH_MUX[SRC]` (8-bit, 256 lines), serviced from a **bottom half** — never inline, or the DMA's write back into the requesting peripheral is a re-entrant MMIO access QEMU **drops silently** while the channel still reports DONE. Real sources today: **LPUART1/2 Tx+Rx** (`SRC` 16–19). **Channel linking**: minor-loop (`CITER[ELINK]`/`LINKCH`, and CITER is only **9 bits** when ELINK is set) and major-loop (`TCD_CSR[MAJORELINK]`/`MAJORLINKCH`). **Scatter-gather**: `TCD_CSR[ESG]` fetches the next TCD from `TCD_DLAST_SGA` (which is a POINTER when ESG is set, an address adjustment otherwise). `TCD_CSR[DREQ]` auto-clears ERQ at major completion. Per-ch IRQ (eDMA3 95+, eDMA4 grouped `128 + (ch%32)/2`, matching `DMA4_CH0_CH1_CH32_CH33_IRQn`). Also see `tests/imxrt1180-edma` (CITER>1) and `tests/imxrt1180-dmareq` (peripheral-triggered, over a real wire) |
| **SAI** (I2S audio) | 1..4 | 0x443B0000, 0x42BB0000, 0x42BC0000, 0x42BD0000 | ◐ bring-up | TCSR/RCSR SR+FR resets self-clear, TX FIFO advertises space (FWF), RX empty; init handshake settles; no audio streaming (flagged); IRQ 45/198/199/154; adapted from the MCX SAI |
| **SRC + BLK_CTRL_S_AONMIX** | 1 | 0x44460000 / 0x444F0000 | ✅ functional | M7 boot-vector (M7_CFG) + release (SCR.BT_RELEASE_M7), bottom-half start |
| **FlexSPI** (controller) | 1, 2 | 0x425E0000, 0x445E0000 | ● functional | LUT-driven IP command engine over SSI + AHB/XIP window; real `m25p80` NOR on FlexSPI1 (16 MiB, `-drive if=mtd`). Storage-write-verified: erase→program→read-back byte-exact, and program-without-erase correctly only clears bits. FlexSPI2 has no flash on the EVK, so its AHB window is deliberately unmapped |
| **RGPIO** | 1..6 | 0x47400000, 0x4381/2/3/4/5 0000 | ✅ functional | PDOR/PSOR/PCOR/PTOR/PDDR/PDIR + per-pin qemu_irq out |
| **TRDC** | 1..3 | 0x44270000, 0x42460000, 0x42810000 | ◐ config stub | HWCFG0 counts + per-master DACFG.NCM (fsl_trdc DAC setup); byte-access safe; no access enforcement (flagged) |
| **USBPHY** | 1..2 | 0x42CA0000, 0x42CB0000 | ✅ functional | USB-HS PHY PLL: RW/SET/CLR/TOG register bank; PLL_SIC.PLL_LOCK reported once powered (instant lock, like ANADIG); no UTMI/charger-detect |
| **USB OTG** (device) | 1..2 | 0x42C80000, 0x42C90000 | ◐ device bring-up | ChipIdea USB-HS: CAPLENGTH/DCIVERSION/DCCPARAMS (device+host capable, 8 EP), USBCMD.RST self-clear, USBSTS/ENDPT* W1C; init+run complete. No host attached → no enumeration/transfers (flagged, not faked); IRQ 215/214 |
| **eFlexPWM** | 1..4 | 0x42650000, 0x42660000, 0x42670000, 0x42680000 | ◐ functional | Motor-control PWM. 4 submodules/module; INIT+VAL0..5 double-buffered (commit on MCTRL.LDOK); MCTRL.RUN starts a ptimer-backed reload at modulo/(clk/prescaler); STS.RF + INTEN.RIE raise the submodule reload IRQ (the FOC loop clock); PWMA duty computed (per-mille). SM0..3 IRQ 24-27/171-174/176-179/181-184 + fault 23/170/175/180. No motor plant/encoder responds + ADC-sync XBAR routing not modelled (flagged) |
| **EQDC** (encoder) | 1..4 | 0x42710000, 0x42720000, 0x42730000, 0x42740000 | ◐ functional | Quadrature decoder. CTRL.LDOK self-clearing load (SWIP preloads UPOS:LPOS from UINIT:LINIT); coherent 32-bit read (UPOS read snapshots LPOS/REV/POSD → hold registers); register-accurate position/rev/diff counters. IRQ 185-188. No encoder/plant drives the inputs → counters do not advance on their own (flagged) |
| **LPADC** | 1..2 | 0x42600000, 0x42E00000 | ◐ functional | 12/16-bit SAR ADC. CTRL soft-reset/FIFO-reset self-clear; CAL_REQ reports calibration done (STAT.CAL_RDY, GCR.RDY); SWTRIG + 8 HW-trigger inputs run the TCTRL/CMD command chain (CMDH.NEXT), pushing tagged results (VALID/TSRC/loop) into RESFIFO; FCTRL.FCOUNT + STAT.RDY + IE watermark IRQ (93/189). No analog front-end/plant → each result is a fixed mid-scale placeholder (flagged, not a fabricated current) |
| **TMR** (QuadTimer) | 1..8 | 0x42690000 + n*0x10000 | ◐ functional | 4 channels/module, 16-bit; CTRL.CM+ENBL start a ptimer-backed modulo counter (PCS internal /2^n) that compares COMP1 → SCTRL.TCF + shared IRQ (TCFIE), reloads from LOAD; live CNTR. IRQ 0/233/164/151/4/5/6/7. Capture/quadrature/cascade/COMP2 modes fall back to periodic (flagged) |
| **LPTMR** | 1..3 | 0x44300000, 0x424D0000, 0x42CD0000 | ◐ functional | Low-power timer: CSR.TEN starts a ptimer counting the prescaled clock to CMR → CSR.TCF + IRQ (TIE), reload; live CNR. IRQ 18/67/150. Pulse-counter/glitch-filter fall back to periodic (flagged) |
| **GPT** | 1..2 | 0x446C0000, 0x42EC0000 | ◐ functional | General-purpose timer: CR.EN + OCR1 compare → SR.OF1 + IRQ (IR.OF1IE), restart; live CNT (prescaler PR). IRQ 209/210. Capture/OCR2-3/free-run fall back to OCR1 periodic (flagged) |
| **TPM** | 1..6 | 0x44310000, 0x44320000, 0x424E/F0000, 0x42500/10000 | ◐ functional | Timer/PWM module: SC.CMOD + MOD → SC.TOF overflow + IRQ (TOIE), wrap; live CNT (2^SC.PS prescaler). IRQ 36/37/75-78. PWM channel outputs/capture not driven (flagged) |
| **XBAR1** | 1 | 0x42750000 | ◐ functional | Signal crossbar. 111 SEL registers (two 8-bit output-source selects each) route any of 256 inputs to any of 221 outputs; an input level propagates to every output selecting it. Wired eFlexPWM1 trigger outputs → ADC HW-trigger inputs. CTRL[] edge/DMA modes stored but not behaviourally modelled (flagged) |
| **Virtual-motor plant** | 1 | — (behavioural) | ✅ closes the loop | Calibrated **dq PMSM** tying PWM+EQDC+ADC together: reads eFlexPWM1 duty → phase voltages (24 V bus) → Clarke/Park → dq stator-current dynamics (Ld/Lq + cross-coupling + PM back-EMF) → magnet+reluctance torque → integrates rotor velocity/angle → drives EQDC1 position + injects phase currents into LPADC1/2. Parameters from the MCUXpresso M1 motor (Pp=4, Rs=0.54Ω, Ld=336µH, Lq=218µH, Kt=0.0548, J=1e-5); settable constant load torque (`load-mnm`). A FOC loop on the guest spins/holds a virtual rotor and senses it back. Dormant until driven. Saturation/thermal + time-varying load profile are future work (flagged) |
| **FXLS8974** (accel) | 1 | LPI2C2 @0x19 | ✅ functional | EVK on-board 3-axis accelerometer (U115). WHO_AM_I=0x86, SENS_CONFIG1 standby/active handshake → FXLS_Init passes; OUT_X/Y/Z report a fixed "board flat at rest" orientation (+1g on Z). No motion input → board does not tilt (flagged) |
| **SEMA42** (semaphores) | 1..2 | 0x44260000, 0x42450000 | ✅ functional | 16 hardware gates; write domain (master+1) locks a free gate, first-writer wins, write 0 frees; read = owner. Inter-core mutual exclusion. Owner-only-unlock / RSTGT not master-gated (flagged) |
| **CMP** (comparator) | 1..4 | 0x42DC0000 + n*0x10000 | ◐ readiness | C0..C3 control register-accurate; CFR/CFF W1C. No analog front-end → COUT fixed, no edge IRQ (flagged). IRQ 200-203 |
| **VREF** | 1 | 0x42E30000 | ◐ readiness | CSR reports the reference stable once enabled; config register-backed; no analog output modelled (flagged) |
| **USDHC** (SD/MMC host) | 1..2 | 0x42850000, 0x42860000 | ✅ functional | Upstream imx-usdhc controller; real Host Controller Capabilities + command/ADMA engine. Board attaches a QEMU SD card per `-drive if=sd,index=N`; no drive → no card (honest). IRQ 86/87 |
| **Readiness blocks** | — | SINC1-3, SPDIF, PDM, SEMC, I3C1-2, USBNC1-2, MSGINTR1-6, FLEXIO1-2 | ◐ readiness | register-backed config (writes stick + read back) + optional forced ready/idle bit; no data path modelled (flagged). Gives a coherent register view instead of the raw catch-all |
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
- **PWM→XBAR→ADC sync** (`tests/imxrt1180-pwmadc`, `PWM->XBAR->ADC: PASS`) — a
  running eFlexPWM submodule emits an output trigger each period; the XBAR routes
  it (input 74 → output 140) to the LPADC hardware trigger, launching a
  synchronised conversion with no CPU involvement — the FOC current-sense path.
- **Closed FOC loop** (`tests/imxrt1180-motor`, `MOTOR: PASS`) — applying a
  stator voltage vector on the PWM makes the virtual PMSM rotor rotate to align
  with the field: the EQDC position follows it (~CPR/4 for a 90° vector) and the
  LPADC senses a real phase current.  Idle PWM → rotor still, mid-scale current.

> ### ✅ FOC path: VALUE-VERIFIED against first-principles goldens (2026-07-12)
>
> A mutation audit (`tools/mutation-audit.sh` — corrupt the model, see whether the
> test notices) initially found **the entire FOC chain asserted by nothing**: a
> 3×-too-long PWM period, a wrong ADC conversion code, and a 3×-wrong phase
> current *all passed*. Those tests stopped at "an IRQ fired", "a VALID bit was
> set", "the current is within a *range*". **A range is not a golden.** The claim
> was retracted first, then re-earned. The tests now assert **values**:
>
> | mutation applied to the model | before | now |
> |---|---|---|
> | eFlexPWM period **3× too long** | `PASS` 🔴 | **`FAIL` ✔ caught** |
> | LPADC returns a **wrong conversion code** | `PASS` 🔴 | **`FAIL` ✔ caught** |
> | plant reports **3× the phase current** | `PASS` 🔴 | **`FAIL` ✔ caught** |
> | *(control)* eDMA corrupts one byte | `FAIL` ✔ | `FAIL` ✔ |
>
> Five more were added with the peripheral-triggered eDMA path (2026-07-12). None
> of these can be reached by the mem-to-mem eDMA test, because that one triggers
> with `TCD_CSR[START]` — **it passes on a model where `ERQ` is a dead constant
> and no peripheral drives a request line, which is exactly what this model was.**
>
> | mutation applied to the model | `tests/imxrt1180-dmareq` says |
> |---|---|
> | eDMA **ignores `CH_CSR[ERQ]`** (un-armed channel serves requests) | **`FAIL` ✔ caught** |
> | eDMA **ignores the request line** (arming a channel runs it) | **`FAIL` ✔ caught** |
> | eDMA services requests **inline from the peripheral's MMIO handler** | **`FAIL` ✔ caught** |
> | one request runs the **whole major loop**, not one minor loop | **`FAIL` ✔ caught** |
> | LPUART **holds its RX request line asserted** regardless of `RDMAE`/`RDRF` | **`FAIL` ✔ caught** |
>
> The inline-service mutation is the one worth staring at. It is mcxn947qemu's
> re-entrancy trap: the DMA's write back into the requesting peripheral is a
> re-entrant MMIO access and **QEMU drops it silently**. Probed on our model, the
> TX channel then reports `DONE=1`, `ERQ` auto-cleared by `DREQ`, `CITER` reloaded
> to `BITER=32` — **a textbook completed 32-byte major loop, with zero bytes on the
> wire.** A test that checked the channel's own status registers would have passed
> it. Only the *peer* knew the truth.
>
> The goldens are derived **independently of the model**, from the machine's
> clocks and the motor's datasheet — so they check the thing rather than restate it:
>
> - **PWM period** — measured against **SysTick** (an Arm core timer, not one of
>   our peripheral models): `200 MHz / 64 / 1000 = 3125 Hz` → `300e6/3125 = 96000`
>   CPU cycles per carrier period. **Measured 96026 (0.03%).**
> - **Phase current** — predicted from Ohm's law: duties → phase voltages →
>   amplitude-invariant Clarke → `|v| = 1.4965 V`; at alignment the stator is
>   purely resistive, so `|i| = |v|/Rs = 2.771 A`; inverse Clarke → phase B
>   `= 2.400 A` → ADC code offset `8341`. **Measured 8340 (one count).**
> - **Rotor alignment** — an electrical 90° vector at `Pp=4` must settle the
>   encoder at `4096 × 22.5/360 = 256` counts. **Observed exactly 256.**
>
> Two things fell out of doing this. The plant's physics is **vindicated** — it
> matches an independent derivation to one ADC count. And the *old* test was
> reading a **transient**: it sampled before steady state and its range check
> passed on an unconverged value. A range check hides an unconverged number as
> readily as a wrong one.
- **Board-to-board UART** (`tests/imxrt1180-uartlink`, `UARTLINK: PASS`) — LPUART2
  on a socket chardev links to a peer with a resend-until-connected GO handshake,
  then a 32-byte pattern echoes byte-exact.  Wires the RT1180 into holobench as a
  b2b node (the fleet's proven UART transport).
- **Bubble** (`bubble_peripheral`) — the FXLS8974 on LPI2C2 initialises and the
  demo reads a level orientation (`x=0 y=0`).

## Known gaps (surfaced by the demo corpus)

### ⚠️ Retraction: the eDMA register map was wrong, and every eDMA test passed anyway

**2026-07-12.** Four separate defects in the eDMA, all found in one afternoon, none
of which any test in this repo could have caught:

| what was wrong | what it should be | why no test saw it |
|---|---|---|
| Channel `n` at `base + 0x1000*(n+1)` | DMA3: `+0x10000 + n*0x10000`; DMA4: `+0x10000 + n*0x8000` | **The tests used the model's own addresses.** They could only ever confirm the model agreed with itself. |
| `TCD_CSR[START]` ran the **whole major loop** | one **minor loop** — it is a *service request*, same as a hardware request (RM 5.4) | **Every eDMA test used `CITER=1`**, where one minor loop *is* the whole major loop and the two models are bit-identical. |
| `CITER` masked with `0x7FFF` always | `0x1FF` when `ELINK` is set (bits 9–14 are `LINKCH`) | Nothing ever set `ELINK`. |
| M33 TCM had **no DMA-visible alias** | CTCM `0x0FFE0000` → **`0x201E0000`**; STCM `0x20000000` → **`0x20200000`** | Every test buffer was in OCRAM, where local and DMA addresses coincide. |

The register-map bug is the one to sit with. **The model was adapted from the
MCXN947's eDMA and its channel geometry was never re-derived from the RT1180 CMSIS
header** — and because the *tests were written against the model*, the whole eDMA
test suite was green on a peripheral whose registers were at the wrong addresses.

> **The stock NXP driver hung immediately.** It takes its addresses from the CMSIS
> header, so its DMA4 channel-0 writes landed on our channel *15*, and the completion
> interrupt went to NVIC 135 while the guest waited on 128.
>
> ### That is the entire argument for running the vendor's driver. A test you wrote against your own model is an oracle you wrote yourself.

`CLAUDE.md` has said all along: *"Never fabricate register offsets… derive them from
the CMSIS header."* The offsets **within** a channel block were derived. The **block
base and stride were inherited from a sibling chip**, and nobody thought of those as
"offsets" — so the rule was obeyed exactly where it was easy and skipped where it
mattered. **Read the whole struct, not the fields you happened to be looking at.**

Now: **9 of 9 stock NXP eDMA `driver_examples` pass, checked on the DATA** — and that
last clause is not decoration. Before scatter-gather was modelled, `edma4/scatter_gather`
printed *"example finish"* over a destination buffer of `1 2 3 4 **0 0 0 0**`: half the
transfer silently missing. **The first version of the sweep that "verified" this fix
grepped for the word `finish` and scored it PASS.**

### Which peripherals drive a DMA request line

The eDMA's peripheral-request path exists and is verified (see the eDMA row), but
a request path is only real for the peripherals that actually **assert a line**.
Today that is **LPUART1 and LPUART2 only** (`CH_MUX[SRC]` 16–19).

Every other modelled block that is a DMA source on real silicon is **PIO-only
here**: `LPSPI1/2` (SRC 11–14), `LPI2C1/2` (7–10), `SAI1–4`, `FlexCAN`, `LPTMR1`
(15), `LPADC`, `eFlexPWM`, `RGPIO`. Their register models are correct and their
tests pass, and a driver that moves data through them **by CPU** works — but a
driver that configures eDMA and waits for it will **wait forever**, because
nothing ever asks. That is a hang, not a silent wrong answer, so it is honest;
it is still a gap.

> **This is how the gap was missed for so long, and it is worth naming.** The
> eDMA row said "TCD-driven mem-to-mem … START triggers" and the SAI row named
> the missing "SAI FIFO→eDMA hardware-request handshake". **Both were accurate.**
> The failure mode is a *correctly-flagged gap you stop thinking about BECAUSE
> you flagged it*: it was written down in ONE row when it was a gap in TWELVE, and
> a per-row flag never adds up to the cross-cutting statement. Hence this table —
> the claim now lives where its scope actually is.
> (Found by mcxn947qemu sweeping the fleet: `grep -n 'ERQ|DMAMUX|dma_req' your_dma.c`
> — *"if ERQ is defined but never read, or no peripheral drives a request line,
> your DMA-driven drivers hang and your source peripherals' data paths are PIO-only."*
> Ours had `CH_CSR_ERQ` defined at line 40 and **never read**: a dead constant.)


| Needed for | Block | Base | Status |
|-----------|-------|------|--------|
| sai | **audio codec + SAI↔eDMA streaming** (the eDMA request path now EXISTS — see the eDMA row — but SAI does not yet drive its FIFO request line; only LPUART1/2 do) | — | the stock `sai` demo is a codec loopback (SAI1 + DMA3 ch0/1 muxed to SAI1 Tx/Rx + a WM8962-class I2C codec). Needs the SAI FIFO→eDMA hardware-request handshake *and* a codec model; the demo's assert is codec-dependent, so it is deferred rather than faked |
| usb_device_dfu | **USB host enumeration** | 0x42C80000 | controller inits + runs; no host attached, so the device does not enumerate (bridging to QEMU's USB host framework is future work) |
| multicore_trigger | **boot-ROM AHAB container parse** | 0x38001000 | `BOARD_GetCore1ImageAddrSize` walks an AHAB container (tag 0x87) at FlexSPI+0x1000 for the CM7 image; our loader places the plain cm33 `.bin` in code-TCM and never populates that container. Needs boot-ROM container loading + the paired M7 image (the demo ships only the cm33 blob) — not faked |
| motor-control frontier | ✅ **done** | — | eFlexPWM + EQDC + LPADC + PWM→XBAR→ADC sync + a calibrated dq PMSM plant: a FOC loop closes in emulation. Stretch: saturation/thermal effects + a time-varying load-torque profile |

## Roadmap

The motor-control frontier is underway: **eFlexPWM** (PWM1-4, double-buffered
compare registers + the periodic reload interrupt a FOC loop runs on) and the
**EQDC** quadrature encoder (position/rev counters + coherent read) and the
**LPADC** (command-chain conversion + result FIFO), the **XBAR**-routed
**PWM→ADC synchronised trigger**, and a first-order **virtual-motor plant** are
all modelled — so a field-oriented-control loop now closes in emulation: a stator
voltage vector on the PWM spins a virtual PMSM rotor, the EQDC reports its angle,
and the LPADC senses the phase current.  This is the distinguishing capability
none of the fleet has.  Remaining stretch on this track: a detailed dq/back-EMF
PMSM with calibrated parameters (and a load-torque profile) in place of the
lumped first-order plant.

Remaining demo-corpus items are reference-blocked (fidelity-first): the SAI
`sai.c:467` assert semantics have no source in the cache, and the
`bubble_peripheral` FXLS8974 sensor driver (register map) is absent.  Optionally,
bridge the USB device controller to QEMU's USB host framework for real
enumeration.

## SDK driver_example sweep (real NXP firmware — the fidelity bar)

Built 47 representative `examples/driver_examples/*` from source (west/armgcc,
`--config debug`/RAM) and ran each on the model. **33 ran correctly** against the
unmodified `fsl_*` drivers, covering every modelled peripheral: eDMA3/4, EQDC,
FlexCAN, FlexIO, GPT, I3C, LPADC, LPI2C, LPIT, LPSPI, LPTMR, LPUART,
PDM, RGPIO, RTWDOG, S3MU, SAI, SEMC, SINC, SPDIF, TPM, eFlexPWM. Harness:
`tools/sdk-run.sh`.

**Correction (2026-07-11):** an earlier version of this list also named *FlexSPI*.
That was wrong for the serial-NOR path and is retracted. `flexspi/nor/polling_transfer`
did **not** run — it hung in `flexspi_nor_get_vendor_id`, because the controller was
a readiness stub with no IP command engine. It now genuinely passes (see the FlexSPI
row above and `tests/imxrt1180-flexspi/run.sh`), but it was never validated at the
time it was claimed. Flagging rather than quietly correcting: a peripheral listed as
driver-validated when it wasn't is the same class of false green this repo exists to
avoid.

Fixes the sweep drove: TRDC aperture 0x1000→0x20000 (readback assert unblocked
the whole corpus); GPT `CR.SWR` self-clear + TPM `CONTROLS[]` backing (fsl-audit);
TMPSNS calibration → `tempsensor` reports 25.0 C.

Known gaps (honest — firmware ran and reported these):
- **netc** ✅ (ENETC endpoint): the NETC PCIe Ethernet block is modelled at
  0x6000_0000 — IERB/PCI/capability register semantics, a behavioural EMDIO +
  RTL8201 PHY (link-up), and a TX->RX buffer-descriptor MAC loopback with MSI-X
  completion via the MSGINTR router. NXP's netc_txrx_transfer runs end-to-end
  (20/20 frames, byte-exact). NOT yet modelled: the L2 switch path (SW0_*, FDB),
  multiple/virtual Station Interfaces, and the PTP 1588 timer — follow-on work
  toward the full TSN stack (see `ref-rt1180-tsn-stack`).
- **asrc** (assert): needs the Audio-PLL clock tree brought up (SAI1 root reads
  576 kHz vs the 1.536 MHz the driver requires; `AUDIO_PLL.CTRL0/NUMER/DENOM`
  read 0 — the boot config leaves the audio PLL down in the RAM build) **plus**
  the ASRC sample-rate-converter data path and the WM8962 codec. An audio
  subsystem, not a point fix — not faked.
- **cache** (self-test fail — *documented QEMU architectural limitation, WONTFIX*):
  the XCACHE example demonstrates cache *incoherence* — CPU caches a value, eDMA
  overwrites physical RAM behind it, and the test proves the CPU read the stale
  cached copy until `DCACHE_InvalidateByRange`. QEMU/TCG has no guest-visible CPU
  data cache (memory is always coherent), so there is never stale data and the
  "was it stale?" branch is never taken → the demo reports failure. Fixing it
  would require modelling the Cortex-M33 L1 D-cache inside QEMU's CPU core, which
  QEMU deliberately does not do. **This does not affect real firmware:**
  `DCACHE_Clean/Invalidate` are harmless no-ops on coherent memory, so a
  customer's cache-managed DMA driver works correctly — only this
  incoherence-*demonstration* self-test cannot pass. Not chased by design.
- **mecc** (self-test fail — *optional, low priority*): the MECC example injects
  an OCRAM ECC error and checks the controller detects/corrects it and fires the
  SingleError IRQ. QEMU's OCRAM is plain RAM with no ECC, so injection is inert.
  Modellable (promote the readiness block to a memory-controller that sits in the
  OCRAM path: compute ECC on write, honour the injection registers on read, latch
  syndrome/address + raise the IRQ) — ~a day — but it is a RAS fault-injection
  diagnostic most firmware never exercises. Left as a readiness block; revisit
  only if ECC fault-injection becomes a user requirement.
