# RT1180 NETC node for the fleet's 3-node cross-silicon L2 segment

`netc-lab3-0x88B6.elf` — the **committed artifact** for node `0x88B6`.

`tools/netc-eth-lab3.sh` builds this same firmware by patching the MCUXpresso SDK's
`netc/txrx_transfer` example at run time and dropping the ELF in `/tmp`. That is fine
for a developer with the SDK extracted; it is **useless to a board farm**, which has
neither the SDK nor a reason to trust a binary that did not exist five seconds ago.
So the artifact lives here, in git, and this file says exactly what it is.

## Launch line (verbatim — this is what was run)

```sh
qemu-system-arm -M mimxrt1180-evk -display none -monitor none \
    -semihosting-config enable=on,target=native \
    -kernel tests/imxrt1180-netc-lab3/netc-lab3-0x88B6.elf \
    -nic socket,mcast=230.0.0.9:31337,mac=54:27:8d:00:00:00 \
    -serial stdio
```

No `-global` and no other flags. The console is LPUART1 on `-serial stdio`; the PASS
token is printed by the **firmware**, not by the harness.

| | |
|---|---|
| EtherType | `0x88B6` (peers: mcxn947 `0x88B5`, i.MX 95 `0x88B7`) |
| MAC | `54:27:8d:00:00:00` |
| MAC IP | NETC (ENETC endpoint) |
| Core | Cortex-M33, bare metal |
| PASS token | see the machine-readable `PASS-TOKEN:` line below — **match the prefix, not the whole line** (the firmware appends a sequence counter). |
| ⚠ do NOT grep | `ENET-LAB3 up: ... need 0x88b5 + 0x88b7` — the banner **contains the peer EtherTypes**. A monitor that greps for a peer ID matches its own banner and shouts PASS at an empty wire. The observer must not put itself in the set it is observing. |

## The three properties a lab runner needs, and how each was checked

**1. IT IS PERSISTENT. There is no `SYS_EXIT` after PASS.**

`cpu0_main()` ends in `for (;;)` — it beacons forever, announces PASS **once**, and
keeps broadcasting so a peer arriving later still sees it. QEMU stays up for the
process's whole life, so a QMP connect will not be refused.

> This matters more than it looks. A firmware that semihosting-exits on PASS
> **terminates QEMU** — and then *"left early"* and *"crashed"* are the same
> observation to anything watching from outside. **Departure must be something the
> lab SCHEDULES, never something the firmware does on its way out the door.**
> (holobench, 2026-07-13, who hit exactly this onboarding us.)

Verified: run it alone and kill it at 20 s → **exit 124**, i.e. it was still running.

**2. IT REFUSES TO PASS ALONE.** Run into an empty segment it prints its banner and
then nothing, forever. It must observe **both** peers *by EtherType* before it claims
anything, and it ignores its own EtherType on RX — a QEMU mcast socket hands your own
broadcast back to you, and counting it is how you "see a peer" that is yourself.

Verified: solo run → banner, no PASS.

> That assertion declined a green **four times** across three sessions, every time on
> a live wire with every appearance of victory — including once when our own watch
> script matched a **substring of our own banner** (`"need 0x88b7"`) and announced
> *"imx95 is on the wire!"* twelve times against zero peer frames. **The firmware
> refused anyway.** Grep the token, never the substring — including in the throwaway
> one-liner you write to watch your own success.

**3. IT TOLERATES JOINING A SEGMENT THAT IS ALREADY LIVE.** Yes — and this is the
whole reason the node exists in this shape.

`netc_can_receive()` used to return false until the guest had programmed its RX ring.
**When a QEMU `NetClientInfo.can_receive` returns false, QEMU STALLS that peer's queue
and never retries** — the device must call `qemu_flush_queued_packets()` when it can
accept again. We did not, so a frame arriving between NIC-up and RX-ring-programmed
**stalled the queue permanently** and that instance went deaf for the entire run.
Fixed in `hw/net/imxrt1180_netc.c` (flush on the `RBLENR` write).

> **No 2-node test can reach that bug, and no *synchronous* 3-node test can either.**
> Both ends boot together, so neither transmits before the other listens. **The window
> opens only for a node joining traffic ALREADY IN FLIGHT.** It presented as *"two
> nodes are deaf, the third is fine"*, and *which* node survived varied run to run.
>
> ⇒ **The bug class lives in TIME, not TOPOLOGY.** A lab that starts every node at
> once will be green forever and will never find anything again.

## The one thing that is NOT tested

**Leaving early.** This node has never been the one that departs mid-run while the
others keep going. It is persistent by construction, so a lab must kill it from
outside — and what the *surviving* peers do when a beacon they were counting on stops
is, as far as this repo knows, **unexercised**. Stated here rather than assumed.

## Rebuilding it

```sh
tools/netc-eth-lab3.sh          # self-test: 3 stand-ins on a private mcast group
MCAST=230.0.0.9:31337 tools/netc-eth-lab3.sh --join
```

Needs the extracted MCUXpresso SDK (see `CLAUDE.md`). The committed ELF is what that
script produced for `0x88B6`; if you rebuild and it differs, the SDK moved.

## The contract a consumer must match

A board farm asserts on the node's own PASS token, verbatim. So the token is declared
ONCE, here, in a form a machine can read without guessing — and `check-token.sh` holds
the ELF to exactly this string:

    PASS-TOKEN: ENET-LAB3 PASS #

Match that **prefix**. The firmware prints `ENET-LAB3 PASS #<n>: saw BOTH peers on the
segment`, and `<n>` increments on every re-arm.

**Do NOT grep the startup banner.** It reads
`ENET-LAB3 up: rt1180 ethertype 0x88b6, need 0x88b5 + 0x88b7` — it **contains the peer
EtherTypes**. A monitor that greps for a peer ID matches its own banner and reports
success at an empty wire. (Ours did. Twelve times.) **The observer must not put itself
in the set it is observing.**

## ⚠ The PASS token in this file was WRONG until 2026-07-13

It documented `ENET-LAB3 PASS: saw BOTH peers on the segment`. **That exact string does
not exist in the binary** — the firmware prints `ENET-LAB3 PASS #<n>:`, with a counter.
A board farm that asserted on the documented token verbatim (which is the *correct*
discipline: grep the token, not a substring) would have scored this node **RED on a
segment where it was passing**.

Nothing was checking that the doc and the artifact agreed. Caught only because holobench
said it would take the token *verbatim from this file*.

  ⭐ **A PASS TOKEN IS AN INTERFACE. THIS ONE WAS DOCUMENTED IN ONE FILE AND EMITTED
     FROM ANOTHER, AND NOBODY DIFFED THEM.**

The counter is a feature, not noise: it turns "did it pass *again* after the late peer
arrived?" into an assertion on **a number going up**, never on the absence of a message.
`PASS #1` at t+210 and `PASS #7` at t+400 says the node is *still actively satisfied* —
not that it once was.

## The payload assertion — the frame BODY, not just its label

holobench, 2026-07-13, on a lab that was green:

> *"Still open and still the biggest hole: the verdict keys on ETHERTYPE. Your 88
> frames DMA'd to guest address 0 would STILL be invisible to this run. The checkable
> payload is the last thing between us and a lab that can be trusted when it is green."*

Correct. An EtherType survives a corrupted frame body, so a node can report *"I saw
both peers"* over garbage — and ours did, 88 times, while passing.

⚠ **AND A CHECKSUM DOES NOT FIX IT.** When the RX path drops a frame it leaves the
descriptor pointing at a **stale buffer** — which holds a *previously valid* frame,
with a *perfectly valid* checksum. The corruption is not a mangled frame. **It is an
OLD one, delivered again.** (This is why our first attempt — checking the source MAC —
was wrong, and had to be retracted.)

⇒ **A MONOTONIC SEQUENCE NUMBER.** Every beacon carries `"LB3!"` + a per-sender counter
that increments on every TX. The receiver asserts it **strictly increases, per peer**:

- a **stale buffer REPLAYS** an old seq → it goes **backwards** → caught.
- a **dropped** frame makes it jump **forwards** → fine, and honest.

Same discipline as the heartbeat: **assert on a number going up.**

### It is a real assertion, and here is the proof it can fail

Restore the pre-`91ccabb9c8` model (gate `can_receive`/flush on `RBLENR` instead of
`RBMR[EN]`, and remove the ring-full check) and run the same staggered join:

```
BUG RESTORED:   PASS heartbeats 11,625   ← STILL PASSES
                PAYLOAD-REPLAY     26    ← and this is what sees it

  ENET-LAB3 PAYLOAD-REPLAY: peer 0x88b5 seq 12173 <= last 12175
                            -- the RX path delivered a STALE BUFFER

FIX IN PLACE:   PASS heartbeats 11,420
                PAYLOAD-REPLAY      0
```

**The node PASSES under the bug and the payload check FAILS under it.** That gap is the
whole point: a verdict keyed on EtherType is structurally incapable of seeing the class
of bug this repo has spent the day fixing.

A lab runner should score **both**: `ENET-LAB3 PASS #` rising *and* zero
`ENET-LAB3 PAYLOAD-`. Either alone is a partial oracle.
