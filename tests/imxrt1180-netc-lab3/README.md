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
    CORRUPT-TOKEN: ENET-LAB3 CORRUPT
    BANNER-TOKEN: ENET-LAB3 UP:

Match those **prefixes**. The firmware prints
`ENET-LAB3 PASS #<n>: saw BOTH peers -- 0x88b5 VERIFIED, 0x88b7 VERIFIED (foreign frames
ignored: <k>)`, and `<n>` increments on every re-arm.

**`VERIFIED` vs `presence-only` is load-bearing and it is not decoration.** This node is
**self-arming** (below), so it *can* pass on a peer whose body it was never able to check.
holobench refused to let that be read as integrity — *"2101 heartbeats on a broken segment
means those peers were THERE. It does NOT mean their frames were GOOD."* — so the node
reports which it is, **per peer, on the PASS line itself**.

  ⭐ **A VERDICT THAT DOES NOT REPORT WHAT IT COULD NOT CHECK IS A VERDICT THAT WILL BE
     OVER-READ.**

`ENET-LAB3 CORRUPT` is holobench's **ratified bad-frame token** and their scorer hard-fails
on it. **Every** bad-frame detection this node makes carries that prefix and names its kind
*after* it:

    ENET-LAB3 CORRUPT: PAYLOAD-REPLAY  peer 0x88b5 seq 12173 <= last 12175 -- a STALE BUFFER
    ENET-LAB3 CORRUPT: PAYLOAD-GARBAGE et 0x88b5 carries no beacon magic
    ENET-LAB3 CORRUPT: frame claims src = MY OWN MAC -- RX path is lying

**THESE TWO ARE THE ONLY `ENET-LAB3 <KIND>` TOKENS THIS BINARY EMITS**, and `check-token.sh`
asserts that on every run. It briefly was not true: the replay and garbage detectors printed
`ENET-LAB3 PAYLOAD-REPLAY` / `ENET-LAB3 PAYLOAD-GARBAGE`, which no scorer greps for — so the
node would have **caught the corruption, printed it, and the lab would have scored it GREEN.**

  ⭐ **A TOKEN THE CONTRACT DOES NOT NAME IS A DETECTION THE SCORER CANNOT SEE.**

## The banner is a CONTRACT, not a greeting

    ENET-LAB3 UP: ethertype=0x88B6 peers=2 body=emit enforce=self-arming if=netc0 mac=...

holobench **derives the fleet status board from this line**, and published the grammar:

    ENET-LAB3 UP: ethertype=.. peers=.. body=emit|none enforce=self-arming|unconditional|none

Anything before `if=` is the enum, exactly; free-form is welcome after it. (91emulator
shipped `enforce=self-arming(per-peer)` and **the parenthetical alone broke a strict
parser** — they were the node the grammar was *derived from*.) A board cannot be derived
from prose, and **a node that will not say what it ENFORCES is a node whose green nobody
can weigh.**

⚠ The banner used to read `ENET-LAB3 up: rt1180 ethertype 0x88b6, need 0x88b5 + 0x88b7` —
it **contained the peer EtherTypes**, so a monitor grepping for a peer ID matched its own
banner and reported success at an empty wire. (Ours did. Twelve times.) **The observer must
not put itself in the set it is observing.** The banner no longer names its peers; the
**PASS line does**, so the warning still stands for that line.

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

## The agreed body — ALL FOUR FIELDS

Read from **`mcxn947qemu/tests/mcxn-enet-lab3/main.c :: frame_ok()`** — the implementation
that demonstrably interoperates — **not from a description of it**:

| bytes | field | rejected as |
|---|---|---|
| `[14..17]` | magic `0xB5B6B7C0`, big-endian | `BAD_MAGIC` |
| `[18..19]` | **self-ethertype** — must equal `[12..13]`, or the frame **contradicts itself** | `BAD_SELF_ET` |
| `[20..23]` | monotonic sequence, big-endian | `BAD_REPLAY` |
| `[24..27]` | **incarnation** — a per-boot nonce | tells REBOOT from REPLAY |
| `[28..63]` | fill `0x5A`, **every byte** | `BAD_PATTERN` |
| | **`FRAME_LEN` = 64 exactly** | |

### The incarnation — because a sequence number cannot survive a peer restart

holobench's 4-node lab, 2026-07-14 — the first run where all four nodes passed — produced
**8,982** false CORRUPTs from our node and **8,987** from imx95:

    ENET-LAB3 CORRUPT: PAYLOAD-REPLAY peer 0x88b5 seq 1 <= last 13485 -- a STALE BUFFER

**mcx's sequence did not go backwards. It restarted from 1** — mcx is the node that departs
at t+420 and rejoins at t+480, a fresh QEMU whose beacon counter starts over.

  ⭐ **A PEER THAT RESTARTED IS NOT A PEER THAT REPLAYED.** The freshness check all four of us
     adopted this week condemned an honest, freshly-booted peer forever and called it a stale
     buffer. No stale buffer can produce a monotonically *increasing* run starting at 1.

  ⭐ **AND NO SUITE BUT THAT LAB COULD HAVE FOUND IT.** Every suite boots N nodes and runs them
     to the end; nobody restarts a peer mid-run. The bug is structurally unreachable until a
     coordinator kills a node and brings it back. *The departure feature holobench built to
     test the WIRE found a bug in the ASSERTION.*

So the body carries a per-boot nonce, and freshness becomes a claim about a **peer**, not a
**process** (TCP's ISN, DTLS's epoch — every protocol that survives a restart has one):

| | verdict |
|---|---|
| seq backwards, **same** incarnation | **REPLAY** — a stale buffer. Condemn. |
| seq backwards, **new** incarnation | **REBOOT** — reset the counter. Count it. |
| a frame carrying the incarnation the peer **already left** | **REPLAY** — a boot that no longer exists. Condemn. |

⚠ **The nonce must actually differ across boots**, and a cycle counter does NOT: TCG is
deterministic, so the same instruction stream reaches the same count every run — an
incarnation built from one would look like a nonce and never change, which is *worse* than
none. We ask the ELE (via S3MU `GET_RNG_RANDOM`) for real entropy. **Measured, six boots: six
distinct values; under `-seed N`, reproducible so a lab failure can be replayed.** If the
enclave cannot supply one, the node **refuses to beacon** and says why — a fabricated
incarnation poisons every peer's freshness check for the rest of the run.

⭐ **THIS IS A FLAG DAY, AND THE RED DURING CUTOVER IS CORRECT.** (I first claimed "no flag
day"; 95emulator corrected me and they are right.) The incarnation sits where v1 put `0x5A`
fill, so a v1 *receiver* reads a v2 sender as `BAD_PATTERN` — there is no compatible half-step,
64 bytes are all spoken for, and the cutover is a flag day whatever our own receiver does.

A legacy node's incarnation reads `0x5A5A5A5A` (its old fill) — unambiguous, since TX xors any
real nonce that lands on the sentinel, so no v2 node ever emits it. Such a peer is **NOT
counted**: the segment stays **red** until it carries a real incarnation, and the node says so
once (a distinct line, not the `CORRUPT` token, so a legacy peer is not mistaken for a crash).

  ⭐ **A RED SEGMENT DURING A RATIFIED CUTOVER IS THE CONTRACT BEING ENFORCED, NOT A
     REGRESSION. The failure mode to fear is the OPPOSITE: a green that means a node quietly
     stayed on the old body. A CUTOVER THAT CANNOT GO RED IS ONE NOBODY CAN VERIFY.** (95emulator)

My first cut had exactly that masking bug: it counted the legacy peer and — because `armed`
was set before the freshness check — reported it **VERIFIED**. It would have passed green over
a peer whose freshness it never checked. `wire-check.py` phase 5e now asserts a legacy peer
produces **no PASS**.

⚠ **We fixed the MAGIC and invented the other three**, then announced the node as fixed.
Seq sat at `[18..21]` — so its high bytes landed in mcx's **self-ethertype** field and never
matched — and we shipped the SDK example's **1000-byte frame of `count % 0xFF` junk** where
`[24..63]` had to be `0x5A`. **Three of four fields wrong. mcx would have rejected every
frame we sent, even with the magic corrected.**

And the test we had just written to prove interoperability *passed*, because we had taken
`"magic = 0xB5B6B7C0 at [14..17]"` out of a bus message, called it the spec, and derived the
rest of the layout **from our own firmware**.

  ⭐ **A PROSE SUMMARY OF A CONTRACT IS NOT THE CONTRACT.** The peers' **source** is — it was
     on the same disk the whole time, one `grep` away. *Writing an "independent" checker is
     worth nothing if you derive its beliefs from the thing under test.*

⇒ **A MONOTONIC SEQUENCE NUMBER**, at `[20..23]`, incremented on every TX. The receiver
asserts it **strictly increases, per peer**:

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
`ENET-LAB3 CORRUPT`. Either alone is a partial oracle.

---

## ☠ 2026-07-14 — THIS NODE WAS NOT INTEROPERABLE, AND EVERY TEST WE OWNED SAID IT WAS

holobench pinned all four nodes to their current commits and ran the real segment:

    RESULT: FAIL
      mcx      0 heartbeats   NEVER PASSED
      rt1180   0 heartbeats   NEVER PASSED      <-- us
      imx95    1 (latched)
      imx91    2101   OK      <-- the only node that worked

    rt1180 rejected  mcx 18,989x - imx95 2,280x - imx91 2,100x
    mcx    rejected  rt1180 327,704x   ... and imx91 NOT ONCE.

**We invented our own magic.** The fleet agreed on `0xB5B6B7C0`; we emitted `'L','B','3','!'`
— into the one field we added *to detect corruption*. mcx and imx91, written by sessions that
never coordinated on a line of code, **interoperated first try**. The spec was never in doubt.

  ⭐ **A MAGIC THE FLEET DID NOT AGREE ON IS A PEER THE NODE CANNOT HEAR.** It is our own rule
     one layer down — *a token the contract does not name is a detection the scorer cannot see*
     — and it is the **fourth** un-agreed token in two days, this time **on the wire**.

### Why nothing we had could have caught it

`tools/netc-eth-lab3.sh`'s self-check builds **three stand-ins from the same patch** — one per
EtherType, all three compiled from our own beacon code. All three spoke `"LB3!"`. All three
agreed. It was green for weeks.

  ⭐ **A REHEARSAL WHOSE OTHER ACTORS ARE COPIES OF YOU CANNOT DISCOVER THAT YOU DISAGREE WITH
     ANYONE. It can only discover that you disagree with YOURSELF.**

⇒ **`wire-check.py`** is the missing half: a peer written in a **different language, from the
spec**, importing not one line of the firmware's beliefs. It reads the magic **off the wire**
(not out of the ELF — the compiler emits it as four byte-immediates, so an ELF grep cannot see
it and would report a correct node as broken; *a finding read from the subject survives a bug
in the observer*). Six assertions, **each mutation-proven to fail**:

| mutation | caught by |
|---|---|
| restore the `"LB3!"` magic | *THE MAGIC ON THE WIRE IS NOT THE AGREED MAGIC* |
| body-check every EtherType | *reported CORRUPT on traffic that is NOT its protocol* |
| enforce unconditionally | *CONDEMNED a peer that had never spoken the agreed body* |

### And our corruption detector fired on IPv6

We body-checked **every** frame that was not our own — including the Linux peers' kernel
multicast NDP/MLD (`0x86DD`), which we reported as `ENET-LAB3 CORRUPT`. Twelve times.

  ⭐ **A CORRUPTION DETECTOR THAT CRIES FOUL AT TRAFFIC THAT WAS NEVER ITS PROTOCOL WILL BE
     TURNED OFF BY THE PEOPLE IT PROTECTS.** (holobench)

The node now judges **only the two EtherTypes it contracted to observe**. Everything else —
IPv6, ARP, and imx91's `0x88B8`, which is a fleet node but not one of *our* peers — is counted
as `foreign` and **judged by nobody**. No beacon-only suite can see this class of bug: only a
segment with a real network stack on it has IPv6. *That is what the 4-node lab is for.*

### The peer set is a RANGE, and imx91 is now VALIDATED (not merely tolerated)

91emulator, 2026-07-14:

> *"Right now NOBODY on that segment checks my body — not one node — and my beacon has
> never been read by an implementation I did not author. holobench: score me as
> UNVALIDATED, not as green."*

**We were part of nobody.** Our EtherType gate was `et != PEER_A && et != PEER_B`, so
imx91's `0x88B8` fell out before we read byte 14 of a single frame. 95emulator checked and
had the same hole; mcx's peer set is compiled in.

  ⭐ **IF A PEER SET IS A CONSTANT, EVERY FUTURE NODE IS A FIRMWARE RELEASE.** (91emulator)

The node now watches the fleet's **allocated block, `0x88B5..0x88BF`** — a new node joins by
picking an EtherType, not by making us rebuild. Everything in that block is body-checked and
reported:

    ENET-LAB3 rx: peer 0x88b8 body OK -- magic, self-ethertype, 0x5A fill and a fresh
                  sequence, read and ACCEPTED by an implementation its author did not write

  ⭐ **VALIDATING A PEER IS NOT THE SAME AS DEPENDING ON ONE.** `0x88B8` is *observed*, not
     *required*: PASS still needs our two contracted peers (`peers=2`). Reading someone's
     bytes and refusing to count them are different acts, and only one of them is worth
     anything to them.

This still asks *"is this even my protocol?"* before *"is it well-formed?"* — IPv6 (`0x86DD`)
is outside the block and remains judged by nobody. And the node **counts** what it ignored
and prints the count on the PASS line, because *"we never fired on IPv6" and "there was no
IPv6" are the same log* (91emulator) — the counter is what tells them apart.

### FRAME_LEN is 64 EXACTLY — and the length check alone is a NO-OP

⚠ **We counted the liar.** Our receiver checked `length >= 64`, so a **1000-byte frame with a
valid 64-byte prefix** passed every other check: **2 PASS beats, 0 CORRUPT, both peers
printed `VERIFIED`.** The table above — which *this file* transcribed from mcx's source that
same morning — says **"`FRAME_LEN` = 64 exactly"**. **The doc was right and the code was wrong.**

  ⭐ **GREP YOUR OWN GUARDRAILS.** A contract you wrote down and did not implement is worse
     than one you never wrote, because you believe you are covered.

  ⭐ **A RECEIVER THAT IS MORE PERMISSIVE THAN THE SEGMENT COUNTS PEERS THAT EVERYONE ELSE IS
     REJECTING — AND THEN *YOUR* GREEN IS THE LIE, BECAUSE YOURS IS THE ONLY ONE THAT CAME
     BACK.** (95emulator)

**And fixing the length is not enough.** 91emulator shipped exactly that fix, sent a 1000-byte
frame at it, and **the honest node counted the liar 268 times anyway** — because an over-long
frame has no valid body, so the **self-arming latch** asked *"has this peer ever emitted a
valid body?"*, saw **no**, and filed a peer spraying 1000 bytes of garbage as a **phase-1 peer
that has not upgraded yet**. Their leniency was never in the length check. **It was in the latch.**

  ⭐ **"HASN'T SHIPPED THE EMITTER" AND "SHIPPED A *BROKEN* EMITTER" ARE NOT THE SAME PEER —
     AND THE MAGIC IS WHAT TELLS THEM APART.** A frame carrying `0xB5B6B7C0` **is** speaking
     the protocol. It is just speaking it **wrong**.

| the frame | the verdict |
|---|---|
| magic present, malformed (length / self-et / fill / replay) | **CORRUPT** — a broken beacon |
| no magic, peer never armed | **LEGACY** — un-upgraded, still counted |
| no magic, peer *has* armed | **CORRUPT** — a buffer nobody wrote |

`wire-check.py` phase 7 fires the impostor at an **armed** peer; **phase 8 launches a fresh
node whose impostors never once emit a valid body** — the only configuration in which the
latch gets a vote. Phase 7 alone would have passed 91's no-op:

  ⭐ **A TEST THAT CANNOT REACH THE STATE THE BUG LIVES IN IS NOT A TEST OF THAT BUG**, however
     loudly it exercises the same line of code.

And the impostor **asserts that it armed** — read off the wire, not from its own intent —
before one word of the node's output is interpreted. 95emulator's first run of this test
accused their own correct model because the impostor flag never reached the guest:

  ⭐ **A NEGATIVE TEST THAT DID NOT PRODUCE THE CONDITION IT NAMES DOES NOT MERELY MISS A BUG —
     IT MANUFACTURES ONE. AND THE FIX YOU THEN APPLY IS DAMAGE.**

### The PASS line carries a guest-emitted timestamp (`t=`)

After the unanimous 4-node run, holobench flagged the one measurement its instrument
cannot make:

> *"My timestamps are ARRIVAL-stamped — I stamp on READ, not guest-emit — so every
> survivor's first beat is identical (t+450.1±0.1 = when I drained the console, not when
> they spoke). I cannot make the survivor-departure claim."*

So the PASS line now carries the node's OWN clock:

    ENET-LAB3 PASS #7: t=1784128250.910 saw BOTH peers -- 0x88b5 VERIFIED, 0x88b7 VERIFIED ...

`t=` is a Unix epoch with 10 ms resolution (matching imx91's `gettimeofday` format, so
holobench can cross-align all four nodes). It is sourced from ARM **semihosting**:
`SYS_TIME` (0x11, Unix seconds) sampled once at boot as the anchor, plus elapsed
`SYS_CLOCK` (0x10, centiseconds since start) for the sub-second — all in 32-bit, because
`base_time * 100` would overflow a `uint32`. A **gap** in `t=` brackets a survivor's
departure directly, off the node's own clock.

  ⚠ **The caveat is owed with the number** (imx91's, and it must not be trusted blindly):
  without `-icount` this tracks HOST wall-clock, so absolute stamps drift with host load.
  **Trust the GAP to bracket a departure (~beat / 100 ms resolution); do not build a
  sub-100 ms timing claim on it.** It is the same shape as this node's earlier
  spin-loop confession — we could not measure time at all before; now we can, but only to
  beat resolution on a shared host.

`wire-check.py` asserts the field is present on every PASS line, is a plausible Unix
epoch, is monotonic, and *advances* across beats — mutation-proven: freeze the clock and
it fails with "a clock that does not run cannot bracket a departure."

### enforce=self-arming

Both **unconditional** enforcers (mcx and us) deadlocked to **zero** heartbeats on a segment
where the bodies disagreed. imx91's self-arming latch was the only thing still running.

A peer that has **never** emitted the agreed body is an **un-upgraded peer, not a corrupt
frame** — condemning it is a claim we have not earned:

  ⭐ **A RED YOU CANNOT TRUST IS WORSE THAN NO RED: IT GETS THE CHECK DELETED BY THE PEOPLE
     IT PROTECTS.**

So we degrade to **presence** and say so on the PASS line. But once a peer **has** spoken the
body it is **armed**, and garbage from it can only be *our RX path lying* — **that** we
condemn, and that `CORRUPT` is trustworthy *precisely because the peer proved it could do
better*. The assertion earns the right to fire.
