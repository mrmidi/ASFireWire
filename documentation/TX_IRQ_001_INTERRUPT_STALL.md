# TX-IRQ-001 — OHCI interrupt delivery stall

**Status 2026-08-26.** Root cause not established. Failure mode fully
characterised, most candidate causes excluded, one new driver-side hypothesis
open (H1, section 4). Direction is deliberately *survivability-first* rather
than *fix-first*, for reasons in section 10.

**Read section 8 first.** TX and RX do not use the 1 ms watchdog the same way:
RX is unconditionally polled by it (the timer *is* the RX drain), while TX only
refills after 5 ms of interrupt silence, into a ring holding 6 ms. So "RX
survives the wedge" is a misreading — RX never depended on the interrupt. And
TX does not die because the ring drains; it dies because we declare it dead at
16 kicks while the fallback is still carrying it. That is the cheapest thing to
fix and it is now step 1.

Hardware: MacBookAir10,1 (M1), macOS 27.0 beta (26A5421a). Host controller
**Agere FW643 rev 8** (`pci11c1,5901`) in **MSI** mode via
`APCIECMSIController-apciec1`. Device: Focusrite Saffire Pro 24 DSP (DICE),
48 kHz, in=16 out=8, blocking.

Reproduction: Debug build **6-11 min**; Release **~2 h**. Use Debug to iterate.

---

## 1. The death chain

Settled. Every step below is observed, not inferred.

1. One OHCI interrupt condition goes unserviced.
2. The handler never runs, so `IntEvent` is never cleared.
3. MSI is edge-triggered: a message is emitted on the transition *into*
   "interrupt asserted". With the condition permanently asserted there is no
   further transition, so **no further messages are ever due**.
4. The dext goes deaf to everything at once — not just TX. `isochTx`,
   `isochRx`, `RQPkt`, `RSPkt`, `reqTxComplete` all latch and stay latched.
5. RX *appears* to survive because it has a poll drain. TX has none, so TX is
   what visibly dies: 16 silent watchdog kicks -> `IT FATAL`.
6. Recovery then fails, because it needs async transactions (DICE/CMP writes,
   IRM release) and the async path is dead too. The device acks our requests
   (`ackCode=0x1` from the AT descriptor status) — we simply cannot hear the
   responses.
7. CoreAudio eventually calls `StopDevice`.

**Only full teardown + re-enumeration restores delivery** (observed 19:37,
~11 s). `RecoverStreaming` cannot fix it. Note that re-enumeration works
because bring-up writes `IntEventClear = 0xFFFFFFFF`
(`ControllerCoreLifecycle.cpp:286`) — the one thing that actually deasserts the
condition.

## 2. Register evidence at the wedge

Live MMIO via `asfw_read_ohci_register` (confirmed live — CycleTimer advances
between samples):

```
IntMaskSet    0x878783FF    bit31 masterIntEnable SET; every relevant source enabled
IntEventSet   0x04300031    reqTxComplete|RQPkt|RSPkt|phyRegRcvd latched forever
HCControlSet  0x804E0000    linkEnable set
ctrl (IT ctx) 0x00008411    run|active|ack_complete — no `dead` bit, no error
```

`0x878783FF` is byte-identical to `kBaseIntMask | kMasterIntEnable` as computed
from `RegisterMap.hpp`, so the hardware mask and our shadow agree.

## 3. Excluded — do not re-open without contradicting evidence

| candidate | why excluded |
|-----------|--------------|
| The DICE device | The stuck events are purely local: `cycleSynch`, `cycle64Seconds`, `reqTxComplete`. No device behaviour can stop a local timer event from being *serviced*. Device keeps streaming and acking throughout; no bus reset in the ring. |
| Interrupt masking | `masterIntEnable` set, all sources unmasked, at the moment of the wedge. |
| Dead chip | CycleTimer advances; `phyRegRcvd` still latches when the app polls PHY registers. |
| Wedged DMA context | `ctrl = 0x00008411`, no `dead` bit, no error event. Descriptor processing was healthy; only delivery stopped. |
| Producer starvation | `prodMin=0/prodRunMin=0` was the steady state during the healthy run too. The producer freeze is downstream of the IRQ death. |
| MSI Enable being cleared | `IOPCIMSIMessageControl` reads 128 both healthy and wedged — it is a static probe-time snapshot. Hypothesis unsupported by that instrument. |
| MSI re-arm (mask toggle) | Tried at silent kicks 2 and 8 (`8c41b064`). `intEvent` byte-identical before and after, twice. Definitive negative. |
| **Blocked dext queue** *(new 2026-08-26)* | The watchdog timer and the interrupt source are on the **same** queue — `ctx.workQueue`, the service "Default" queue (`DriverContext.cpp:371` and `:380`). Timer callbacks keep running through the wedge and do live MMIO. The queue is not blocked, starved, or deadlocked. |
| **We disable our own source** *(new 2026-08-26)* | `InterruptManager::Disable()` has exactly two callers — `ASFWDriver.cpp:232` (quiesce/teardown) and `ControllerCoreLifecycle.cpp:520` (startup-failure rollback). Neither runs in steady state. |
| Shadow/hardware mask divergence *(new 2026-08-26)* | `kBaseIntMask\|master` computes to exactly the observed `0x878783FF`, and `SeedInitialInterruptMask` keeps the shadow in sync. |

## 4. Live hypotheses

### H1 — our handler holds the condition asserted across real work *(new)*

Compare `references/linux-ohci-firewire-low-level-stack/ohci.c:2059`. Linux
reads `IntEventClear`, writes the clear back **as the very next operation**,
then defers all work (`queue_work`, `schedule_flush_completions`). Window
between read and clear: two register accesses.

Ours (`ASFWDriver.cpp:961` -> `InterruptDispatcher.cpp:14` ->
`ControllerCoreInterrupts.cpp:43`) reads a snapshot, then runs fault handling,
bus-reset notification, `DispatchAsyncInterrupts` and logging, and only **then**
calls `ClearIntEvents(toAck)` — where `toAck` derives from the *original*
snapshot. Any enabled bit that sets inside that window is never cleared. There
is no re-read and no check that the condition is deasserted before returning.

Fits the evidence well:

| observation | explained by |
|-------------|--------------|
| Debug 6-11 min vs Release ~2 h | `-O0` widens the read->clear window by a similar factor |
| M1 dispatch tail 5.1% >= 750 us vs M4 0% | wider dispatch delay = wider window |
| only re-enumeration recovers | it is the only path that does `IntEventClear = 0xFFFFFFFF` |
| mask toggle did nothing | it never cleared the latched events, so no new edge |
| Linux `QUIRK_NO_MSI` for this chip family | read-once/clear-once is only safe on level-triggered INTx, which re-fires on a lingering condition |

**Open objection, unresolved.** At ~2667 isoch events/s a wide window should
wedge in milliseconds, not minutes. Either most in-window events are harmlessly
over-cleared (plausible if `isochTx`/`isochRx` are wired-OR summaries of
`IsoXmit`/`IsoRecvIntEvent`, which our snapshot-based clear also over-clears),
or the wedge needs one of the *sporadic* bits. Notably the frozen state reads
`0x04300031` — exactly the sporadic async/PHY bits. Closing this needs OHCI
latching semantics for bits 6/7, which is a spec question, not a guess.

### H2 — chip stops emitting (FW643 erratum)

Linux carries `QUIRK_NO_MSI` for this family (`ohci.c:344`, applied at
`ohci.c:3701`), pinned to revision 6; ours is revision 8. Documents the chip's
MSI as historically unreliable. On Apple Silicon there is no INTx fallback.

### H3 — macOS/`APCIECMSIController` stops routing

A dext does not take the interrupt directly:
`hardware MSI -> kernel handler -> IOInterruptDispatchSource -> dext queue`.
Two macOS-owned hops a kext never had. Consistent with the failure being purely
in delivery. Supporting circumstantial evidence: M4/macOS 26 survived 1 h 51 min
vs M1/macOS 27 Debug's 6-11 min — but that comparison changes SoC, OS and MSI
controller generation at once, so it is not decisive.

## 5. Instruments

**Use:**
- `asfw_read_ohci_register '{"name":"IntEventSet"}'` x3. Direct MMIO, safe on a
  running stream. Healthy = low bits transient (`0x00100000` <-> `0x00100080`);
  dead = `0x04300031` stuck.
- `systemextensionsctl list` to confirm the dext is alive.
- Live `ConfigurationRead16` of the MSI capability's Message Control word —
  **not yet built**. `HardwareInterface.cpp:92-109` already does config-space
  access, so this is cheap. Note this chip has **no Per-Vector Masking**
  (Message Control bit 8 clear), so there is no MSI Pending bit to read.

**Retired, do not re-run:**
- `ioreg IOPCIMSIMessageControl` — static probe-time snapshot.
- `powermetrics --samplers interrupts` residual — not FireWire-specific (the
  5574/s delta is ~2x the maximum possible 2667/s FireWire MSI rate), and
  edge-MSI means it can only observe the aftermath.
- Watching the MSI vector alone in ktrace — same structural trap: both
  hypotheses predict "the vector stops firing". Only a **correlation** test
  (kernel-side interrupt entries vs our `interruptCount_`) discriminates.

**Agent limitation:** the Bash sandbox returns zero lines silently from
`log show`/`log stream`. Hand the user a ready `log stream ... > file` command.

## 6. Where we sit — the numbers

| | IRQ/s | ring depth |
|---|---|---|
| Apple NuDCL TX | 10 | 800 pkt (100 ms) |
| Apple NuDCL RX | 50 | 800 pkt (100 ms) |
| Apple legacy DCL | 1000 | 800 pkt (100 ms) |
| Linux ALSA | ~1300 (6 pkt idle interval) | period-derived |
| libffado | ~1000-2000 (4-8 pkt) | 128 buffers |
| **ASFW TX** | **1333** | **48 pkt (6 ms)** |
| **ASFW RX** | **1333** | **504 pkt (63 ms)** |

See `documentation/APPLE_FWAUDIO_ISOCH_GEOMETRY.md` for how those Apple numbers
were derived.

**Our TX ring is the shallowest thing in the system by 10x, and TX is exactly
what dies.**

## 7. The coupling problem

One constant wears three hats:

```
IsochQueueGeometry::kPacketsPerCompletionGroup = 6
  |- IsochDmaGeometry::kPacketsPerInterrupt      -> which descriptors get IRQ_ALWAYS
  |- AudioTimingGeometry::kTimingGroupPackets    -> ZTS anchor granularity
  '- the refill quantum
```

Apple keeps these as three different numbers (8 / 160-or-800 / per-packet).
Because ours are fused, "lower the interrupt rate" is a three-variable change
that also coarsens ZTS and the refill quantum. **Splitting them is the
precondition for any interpretable interrupt-rate experiment.**

Latent inconsistency: `IsochService::interruptInterval_{8}` is published into
the TX control block and validated by `ASFWAudioDevice.cpp:520` against
`kTxPacketsPerGroup` (6). It is a *reporting* field — it does not drive
descriptor building — and its default disagrees with reality.

## 8. The refill paths — RX and TX are not symmetric

**Established 2026-08-26 by reading `Poll()` on both sides. This is the most
actionable finding so far.**

The 1 ms watchdog (`kAsyncWatchdogPeriodUsec`, `ASFWDriver.cpp:87`) drives
`WatchdogCoordinator::HandleTick` -> `TickIsochReceive` / `TickIsochTransmit`.
The two directions use it completely differently:

| | primary path | what the 1 ms timer does |
|---|---|---|
| RX | **the timer** | `IsochReceiveContext::Poll()` (`Receive/IsochReceiveContext.cpp:232`) has **no stall gating**. Every tick it reads the cycle timer over MMIO, opens a receive batch and runs `rxRing_.DrainCompleted(...)`. It *is* the drain. |
| TX | the interrupt | `IsochTransmitContext::Poll()` only refills inside the `irqStallTicks_ >= 5` branch. Healthy ticks compare two counters and return. |

**Consequence: "RX survives the wedge" is a misreading.** RX does not survive
because it has a fallback — RX never depended on the interrupt at all. It was
already being polled at 1 ms into a 504-packet (63 ms) ring. TX is the only
direction that genuinely needs the interrupt, and it is the one with 6 ms of
runway.

### The TX fallback is under-provisioned by design

```
watchdog tick                     1 ms
refill only when irqStallTicks_ >= 5   ->  one refill per 5 ms
TX ring = 48 packets                   ->  6 ms of runway
                                           margin = 1 ms = 8 packets
```

One late watchdog tick and the ring holes. And per the simulator, the stream
would otherwise survive interrupt loss indefinitely on that fallback — so **TX
is not dying because the ring runs dry. It is dying because we declare it dead
at 16 kicks while the watchdog is still carrying it.**

### Why 1 ms is the wrong period

- **Too fast to be a detector.** At 750 us interrupt spacing and 1 ms polling, a
  single tick cannot distinguish "no interrupt yet" from "interrupts are dead".
  That is the only reason the 5-tick hysteresis exists — and that hysteresis is
  exactly what turns the TX fallback into a 5 ms refill against a 6 ms ring. At
  a 20 ms poll you would know on the first tick.
- **It shares the queue with the interrupt source.** Both are on `ctx.workQueue`
  (section 3). ~1000 timer callbacks/s are serialised against ~2667 interrupt
  callbacks/s. Every timer tick that runs is a tick the handler is not running,
  which **widens the H1 read->clear window**. The watchdog may be aggravating
  the failure it exists to detect.
- **It is a double path.** RX is drained from both the interrupt
  (`InterruptDispatcher.cpp:35`, `DispatchAsync` -> `rx->Poll()`) and the timer,
  at two different rates, uncoordinated. See the double-path rule in
  `CLAUDE.md`.
- **250x finer than the async deadlines** it also serves (`OnTimeoutTick`,
  250 ms+ retry deadlines).

Note the references keep a non-IRQ path but never free-run at 1 ms: Linux calls
`flush_completions` from the **PCM period**; Apple uses a **64 ms** timer. See
[[linux-apple-irq-silence-guards]].

### The lever this opens

If RX is already fully drained by the timer into a 63 ms ring, the `isochRx`
interrupt buys ~250 us of drain latency and costs **half of all MSI traffic**.
Dropping it takes ~2667 -> ~1333 interrupts/s with no geometry change, no ZTS
change and no shared-memory change, and removes the double path. If H1 is right
and the wedge is a per-interrupt hazard, that halves the exposure.

*Unverified precondition:* RX would drain ~8 packets per 1 ms tick instead of 6
per interrupt group. The ZTS anchor is published from `ConsumePacket` inside
`Poll()` and is `(countedFrame, hostTicks)` computed live, so it *should* be
indifferent to what triggered the drain. **Sim this; do not assume it.**

## 9. TX ring depth — simulator results

**Confirmed in `tools/asfw_sim` 2026-08-26**, not just derived. `derive()` was
extended with `hardware_ring_packets` and `packets_per_group` parameters plus
three constraints it was missing (`hwRing % group`, `hwRing % cadenceBlock`,
`zts % framesPerDataPacket`). Full suite green (104 passed).

### Deepest TX ring at today's `shared=912 / timeline=1024`

| group | IRQ/s | max ring | runway | blocked by |
|---|---|---|---|---|
| 6 | 1333 | **84** | 10.5 ms | `preparationLead <= shared - hwRing` (822 <= 816) |
| 8 | 1000 | 88 | 11.0 ms | same |
| 12 | 666 | 84 | 10.5 ms | same |
| 16 | 500 | 80 | 10.0 ms | same |
| 24 | 333 | 72 | 9.0 ms | same |

The 84-packet figure predicted from the headers is confirmed. **Non-obvious
result: coarser interrupt strides make the ceiling worse**, because the frame
exposure window rounds up to whole groups. Group 24 buys 4x fewer interrupts
and costs 12 packets of ring. This is an argument *against* raising the
interrupt interval as a first move.

### Cost of going deeper

| runway | ring | shared | timeline | note |
|---|---|---|---|---|
| 10.5 ms | 84 | 876 | 1024 | fits today |
| **15.0 ms** | **120** | **1020** | **1024** | **timeline unchanged; shared +12%** |
| 30.0 ms | 240 | 1500 | 2048 | timeline doubles |
| 63.0 ms | 504 | 2556 | 4096 | RX parity |
| 100 ms | 800 | 3736 | 4096 | Apple parity |

All geometrically valid. **120 packets is the sweet spot.**

### Behavioural result 1 — interrupt cadence is nearly free

Sweeping the interrupt stride at today's ring, budgets kept consistent per
stride. `tx_packets_per_group` is the producer wake cadence in the model
(`sim.py:274`).

| IRQ stride | IRQ/s | wakes | frames written | verdict |
|---|---|---|---|---|
| 6 pkt (today) | 1333 | 28194 | 99.4% | healthy |
| 24 pkt | 333 | 8402 | 99.4% | healthy |
| 160 pkt (Apple RX) | 50 | 2791 | 99.4% | healthy |
| **800 pkt (Apple TX)** | **10** | 2058 | **99.0%** | **healthy** |

**133x fewer interrupts costs 0.4% of written frames.** The reason is
`sim.py:307`: the **CoreAudio IO callback is already a producer wake source**,
firing every 85 cycles (10.67 ms) — more often than Apple's TX interrupt fires.
So for *preparation* our isoch interrupt is already close to redundant; we
never noticed because we never ran without it.

Pinned as `scenarios/apple-interrupt-cadence.yaml`.

### Behavioural result 2 — the deep-ring blocker is the SLACK POLICY

A ring of 800 collapses to **0% written at every interrupt cadence, including
our current 1333/s**. So it is not the interrupt. Bisected:

```
deepest healthy ring under today's policy = 312 packets (39 ms)
  -> coverage = 936 packets: the producer must run 117 ms AHEAD of hardware
at ring 324: written drops to 62.7%
```

The cause is in `AudioTimingGeometry.hpp`:

```cpp
kTxPreparationSlackPackets = 2 * kTxHardwareRingPackets;
kTxCoverageLeadPackets     = kTxHardwareRingPackets + kTxPreparationSlackPackets;  // 3 * ring
```

**The required look-ahead scales with ring depth.** Deepening the ring should
buy runway; instead it demands more look-ahead. Preparation is fed by RX replay
observations which are inherently ~now, so a producer cannot build packets from
RX data that does not exist yet.

Every `static_assert` passes for these geometries — **only the behaviour fails.**
`derive()` structurally cannot catch this; it is exactly what the simulator is
for. Section 9's "all geometrically valid" table above is correct and
misleading: valid, but behaviourally dead past ring 312.

Pinned as a deliberately FAILING geometry in
`scenarios/slack-scales-with-ring.yaml`.

### Behavioural result 3 — the fix is one line, and it is already the value

At ring 48, `slack = 2 x 48 = 96`. So today's slack **is** 96 packets; it is
merely *expressed* as a multiple of ring depth. Slack is a refill-latency
budget — "how late may the producer be" — which has nothing to do with how deep
the hardware ring is. Hold it constant and ring depth is unblocked:

| ring | runway | coverage | shared | written | verdict |
|---|---|---|---|---|---|
| 48 | 6.0 ms | 144 | 732 | 99.4% | healthy |
| 120 | 15.0 ms | 216 | 876 | 99.4% | healthy |
| 240 | 30.0 ms | 336 | 1116 | 99.3% | healthy |
| 504 | 63.0 ms | 600 | 1644 | 99.0% | healthy |
| **800** | **100 ms** | 896 | 2232 | **98.6%** | **healthy** |

Apple parity — 100 ms of runway — works, and needs **less** shared memory than
the 3x policy demanded for the same ring (2232 vs 3744 slots).

Combined with Apple's interrupt cadence, all healthy:

```
ring=800  irq=10/s   written=98.6%
ring=504  irq=10/s   written=99.0%
ring=120  irq=10/s   written=99.4%
```

**Apple's full TX profile runs clean in our simulator.** Pinned as
`scenarios/apple-geometry-fixed-slack.yaml`.

### Caveats on the above

- `2 * ring` was presumably written for a reason nobody recorded. The constant
  form is defensible on first principles, but check the history before changing
  it.
- Written fraction drifts 99.4% -> 98.6% with depth. Small, and probably the
  longer cold-start prefill over a 20 s run rather than steady state — but that
  has **not** been separated. Do not quote 98.6% as a steady-state figure.
- The simulator models **preparation**, not descriptor-refill mechanics. It says
  the producer can keep a deep ring fed. It does **not** say `DoRefillOnce`
  will.

### One number to ignore

**Do not read `replay_headroom`.** It goes -422 -> -1790 as the ring deepens and
looks alarming, but `tools/asfw_sim/FINDINGS.md` **F1 falsified that invariant**:
the reader cursor advances per prepared packet, not per packet index, so the
lead-vs-readDelay comparison is not valid. `Geometry.satisfies_replay_invariant`
encodes a disproven claim and is retained only for regression purposes.

Producer-stall tolerance is unchanged across every candidate — all survive the
search ceiling. Note the built-in `cliff` command now also saturates (750 ms),
so the 78 ms cliff in `FINDINGS.md` F2 no longer reproduces at current
constants.

## 10. Direction

### Framing

The wedge may well be chip- or host-side and not ours to fix. Therefore the
goal is **survive an interrupt outage**, not only prevent one. This is the same
structural lesson as Linux's `flush_completions` (called from the PCM period,
not only from the ISR) and Apple's timer-driven TX refill — see
`linux-apple-irq-silence-guards` in memory. Neither stack fatals on interrupt
silence, because neither depends solely on the interrupt.

### What we are explicitly NOT doing

- **Not porting buffer groups.** They solve a linked-list pointer-chasing
  problem that does not exist in our flat descriptor array. See section 5 of
  the Apple reference doc.
- **Not testing at 44.1 kHz to get fewer interrupts.** Packets are emitted one
  per isoch cycle — 8000/s at *every* sample rate. Sample rate changes frames
  per packet (6 -> 5.5125), never packets per second. 44.1 yields exactly the
  same 1333/s. In blocking mode the NO-DATA packets are still packets, still
  descriptors, still counted toward the completion group.
- **Not shipping a lower interrupt rate as "the fix."** Coarsening the stride is
  a legitimate *design* choice — section 9 shows it is nearly free, and Apple
  ships 10/s — but it does not fix the wedge, it dilutes it. If MTBF quadruples,
  Release goes ~2 h -> ~8 h and it will *look* cured. Ship it for the
  architecture, never as the remedy, and keep H1 open regardless.
- **Not pursuing** `ioreg IOPCIMSIMessageControl`, the powermetrics residual,
  or bare MSI-vector presence. All three are recorded dead ends.

### Ordered plan

Reordered 2026-08-26 after the section 8 and 9 findings. Two things changed the
shape of this list: the TX fallback defect (small, local, and the reason an
outage becomes a dead stream instead of a degraded one), and the discovery that
the slack policy — not memory, not the interrupt — is what caps ring depth.

1. **Fix the TX fallback** (two small changes, no geometry impact):
   - refill on **every** watchdog tick while stalled, not every 5th. Takes the
     refill period 5 ms -> 1 ms against a 6 ms ring: margin 1.2x -> 6x.
   - stop declaring the transport dead at 16 kicks while the fallback is
     visibly carrying the stream. The fatal is a policy choice, and per the sim
     the stream survives interrupt loss indefinitely on the fallback.
2. **Make the slack a constant.** `kTxPreparationSlackPackets = 96` instead of
   `2 * kTxHardwareRingPackets`. It is already 96 today, so this is a no-op at
   the current ring — and it is the single change that unblocks every deeper
   ring (section 9, result 3). Do it before step 4, not after.
3. **Return-path probe** (free, no fault needed). Re-read `IntEvent` at the end
   of the handler; count when `IntEvent & enabledMask != 0` on return. If the
   counter climbs during **healthy** operation, H1 is confirmed in seconds and
   we need no reproduction to learn it. Anomaly-only logging per the
   instrumentation rule in `CLAUDE.md`.
4. **Deepen the TX ring.** With step 2 in place the sim is healthy all the way
   to Apple parity: 800 packets / 100 ms runway, `kTxSharedSlotPackets` 912 ->
   2232, `kTimelineSlots` 1024 -> 4096. Land it in stages — 120 (15 ms, shared
   876, timeline unchanged) is the low-risk first step and needs no
   shared-memory contract change at all.
5. **RX: pick one path.** Sim first (does 1 ms drain granularity disturb the ZTS
   anchor?), then drop the `isochRx` interrupt and commit to the timer drain
   that is already doing the work. Halves MSI traffic ~2667 -> ~1333/s and
   removes the double path. No geometry, ZTS or shared-memory change.
6. **Split the fused constant** into `kPacketsPerInterrupt`,
   `kTimingGroupPackets` and the refill quantum, then coarsen the TX interrupt
   stride. Section 9 result 1 shows this is nearly free in preparation terms
   (1333/s -> 10/s costs 0.4% of written frames), because the CoreAudio IO
   callback already wakes the producer every 10.67 ms. Combined with step 4 it
   is Apple's profile.
7. **Revisit the 1 ms watchdog period itself.** Once TX has a deep ring and RX
   is the only polled path, the right period is likely 5-20 ms, on a queue that
   is *not* the interrupt source's. The 5-tick hysteresis disappears with it.
8. **Rescope recovery** as a hard link/controller reset modelled on the
   re-enumeration that actually works and on Apple's `cycleLost` remedy (mask
   all interrupts, PHY reg 1 = 0xFF IBR, `scheduleResetLink()`). Fold in option
   (c) from the comment at `AudioCoordinator::HandleTxTransportFault`: give a
   coordinator its own queue and make recovery async for RX and TX alike, which
   also fixes the RX path's existing inline blocking.

**Superseded:** the earlier note that coarser interrupt strides shrink the max
ring was measured under the `3 * ring` slack policy. With step 2 that coupling
is gone, and the interrupt stride becomes genuinely free. The `apple-original`
profile that was queued as future work is now landed as
`scenarios/apple-geometry-fixed-slack.yaml`.

### Open questions

- OHCI latching semantics for `IntEvent` bits 6/7 (`isochTx`/`isochRx`): truly
  latched, or wired-OR summaries of the per-context event registers? This
  decides whether H1's rate objection dissolves. Spec question — ask, do not
  guess.
- Does a live `ConfigurationRead16` of MSI Message Control differ healthy vs
  wedged? The static ioreg property could never answer this; config space can.
- Correlation test (kernel interrupt entries vs `interruptCount_`) remains the
  only instrument that can distinguish H2 from H3. SIP is disabled on this
  machine, so dtrace/ktrace is available and has never been exploited.
- **Does 1 ms RX drain granularity disturb the ZTS anchor?** Blocks step 3.
  Draining ~8 packets per timer tick instead of 6 per interrupt group *should*
  be immaterial — the anchor is `(countedFrame, hostTicks)` computed live in
  `ConsumePacket` — but `kTimingGroupPackets` is nominally tied to the interrupt
  group, so this needs the simulator, not an argument.
- Was the RX-polls-unconditionally / TX-polls-only-when-stalled asymmetry
  (section 8) deliberate? Nothing in the code says so. If it was, the reason is
  worth recording; if not, it is the root of why only TX dies.

### Also open, unrelated

Teardown abort in `AudioNubPublisher::TerminateNub` (`.ips` from 19:10, slice
`28597a2f`). Not caused by the recent work, but the recent work touches that
teardown region — check the slice UUID before blaming it.
