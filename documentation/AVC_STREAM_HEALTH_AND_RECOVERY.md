# AV/C Stream Health & Recovery — Reference Doctrine and ASFW Design

**Date:** 2026-07-16
**Motivating incident:** AV/C-backend `[TxPrepRange]` log flood + dirty output signal (see §1).
**Scope:** how stream death is *detected*, *tolerated*, and *escalated* on AV/C-class
(FCP/CMP-controlled) FireWire audio devices, across all three reference stacks
(Linux ALSA, libffado, Apple AppleFWAudio.kext) — and what ASFW's shared duplex
coordinator should adopt (FW-64 scope).

Related in-flight work: FW-64..73 (backend generalization), FW-100 (FCP attempt state — §7
touches FCP/bus-reset interaction).

---

## 1. The incident this answers

On the AV/C backend, a live session produced a ~1 kHz flood of:

```
[TxPrepRange] ... reqSpan=6 prepared=6 short=0 ... marginAfter=360 ... expose(truncated)
[PayloadWriter] anomaly lastSample=89154328 completion=14847012 deficitMax=50712
                visitedDelta=6144 writtenDelta=0 withoutPktDelta=6144 ... underExpCallsDelta=12
```

Decoded:

- `short=0` on every line → the anomaly gate (`stoppedShort || frameShort`,
  `ASFWAudioDriverZts.cpp:640`) fired on **frameShort** every wake. The console truncated the
  tail that says so (`... frameShort=1 frameDeficit=... writeEnd=... replay=...`).
- Packet ring was **full the whole time** (`marginAfter=360` = `kTxPreparationLeadPackets`,
  `prepared == reqSpan`). Not a buffer-size problem — no ring size bridges a stranded frame
  cursor.
- `deficitMax=50712` ≈ **1.06 s @ 48 k**: the TX content-frame frontier was stranded >1 s behind
  CoreAudio's write head. `writtenDelta=0`, `withoutPktDelta == visitedDelta`: every frame
  CoreAudio wrote missed its packet → NO-DATA/stale wire → the audible "dirty signal".

**Mechanism:** an RX outage reset the replay ring
(`IsochReceiveContext::ResetReplayEpochForDiscontinuity`, `IsochReceiveContext.cpp:642`).
While replay is down, TX legally degrades to NO-DATA and the content-frame cursor freezes while
CoreAudio keeps writing. The designed exit is the self-heal: replay recovers → `[TxAlign]`
re-projects the cursor (`AlignFrameCursorOnce`, `ASFWAudioDriverZts.cpp:370-393`) → deficit
collapses (observed: `deficitMax=384` residual, then quiet). In the incident the outage
persisted for **minutes**, and nothing escalated.

**The gap:** RX fires a timing-loss callback when an *established* replay dies
(`IsochReceiveContext.cpp:660` → `IsochService.cpp:527`). `DiceAudioBackend.cpp:76` subscribes
(health-gated recovery: probe DICE clock registers; healthy → suppress and let self-heal work;
unhealthy → coordinator restart). **`AVCAudioBackend` never calls `SetTimingLossCallback`** —
it has only bus-reset recovery. A persistent outage on AV/C therefore has no floor: dropped
audio until the device stream spontaneously returns.

Secondary defect: the `frameShort` branch of `[TxPrepRange]` logs per wake (~1 kHz) for one
persistent condition. Should be transition-only or rate-limited.

---

## 2. Linux ALSA (`references/linux-sound-firewire-stack/firewire/`)

Authoritative for AV/C-class = **bebob** (BridgeCo) and **oxfw** (Oxford); the packet layer
(`amdtp-stream.c`) is shared by *all* families including DICE — one doctrine for everyone.

### Detection — CIP integrity at the packet layer, no registers

- **DBC discontinuity** → `-EIO`: `amdtp-stream.c:815`
  (`"Detect discontinuity of CIP: %02X %02X"` — computed vs expected `data_block_counter`).
- **Cycle discontinuity** (missed isoc cycles beyond tolerance) → `-EIO`:
  `amdtp-stream.c:986` (`"Detect discontinuity of cycle: %d %d"`).
- Tolerance exists where hardware needs it: OXFW970 legitimately skips several cycles during
  async transactions — `CIP_JUMBO_PAYLOAD` allows `IR_JUMBO_PAYLOAD_MAX_SKIP_CYCLES`
  (`amdtp-stream.c:977-984`). **Short gaps are normal for some AV/C hardware.**

### Escalation — kill immediately, restart from above

- Any packet error → `cancel_stream()` → `amdtp_stream_pcm_abort()` → `snd_pcm_stop_xrun()`
  (`amdtp-stream.c:1075-1088`, `1963-1977`). No in-kernel self-heal, no retry, no probe.
- Userspace gets `-EPIPE`, calls `SNDRV_PCM_IOCTL_PREPARE` → driver restarts streaming.

### Recovery — full duplex restart, **including CMP break/re-establish (wire-observable)**

- `snd_bebob_stream_start_duplex` (`bebob_stream.c:593`): if `amdtp_streaming_error()` on either
  stream → `amdtp_domain_stop()` + `break_both_connections()` (`bebob_stream.c:602-606`), then
  re-establish CMP (`cmp_connection_establish`) and `amdtp_domain_start()`.
- **Warm-up tolerance:** after CMP establish, devices send NO-DATA for hundreds of cycles
  (~1 s) before multiplexing events with valid SYT; some devices are *strict* about receiving an
  adequate SYT sequence, and a corrupted CMP break can cascade into unrecoverable errors or
  bus reset (comment block `bebob_stream.c:637-643`). Hence
  `amdtp_domain_wait_ready(READY_TIMEOUT_MS)` (`bebob_stream.c:663-666`) — a restart is not
  "failed" because data doesn't flow instantly.
- **Bus reset funnels into the same path** — doctrine comment `bebob.c:303-317`: the driver does
  *not* update streams in the bus-reset handler; the reset-induced DBC discontinuity trips the
  XRUN machinery anyway. One recovery path for everything. The bus-reset handler does only
  `fcp_bus_reset()` (FCP transaction cleanup — see §7).

---

## 3. libffado (`references/libffado-2.5.0/`)

Authoritative userspace AV/C implementation (`genericavc`/`bebob` device classes).

### Detection — pure packet liveness against the cycle timer

`IsoHandlerManager.cpp:321-357`, in the ISO poll loop:

```cpp
// "we use a relatively large value to distinguish between 'death' and xrun"
int64_t max_diff_ticks = TICKS_PER_SECOND * 2;                    // 2 seconds
if (measured_diff_ticks > max_diff_ticks) {                        // since last packet seen
    debugWarning("(%p, %s) Handler died: ...");
    m_IsoHandler_map_shadow[i]->notifyOfDeath();
}
```

Per-handler "time since last packet seen" vs the 1394 cycle timer. **> 2 s = dead.**
Device-agnostic — no registers, no AV/C commands.

### Escalation — two-tier

- Handler death → `m_running = false`; the xrun machinery picks it up and "will eventually time
  out if we are not able to recover" (`IsoHandlerManager.cpp:358-365`). A dead handler is
  *restartable* — death is not fatal on its own, it just forces a re-sync attempt.
- Per-period iso xrun/error → `xrunOccurred()` / `inError()` bubble up through
  `StreamProcessorManager::waitForPeriod` (`StreamProcessorManager.cpp:1304-1352`). An *error*
  sets `m_shutdown_needed = true` (fatal); a bare *xrun* is recoverable.

### Recovery — in-place resync with bounded retries

`StreamProcessorManager::handleXrun` (`StreamProcessorManager.cpp:1158`): disable stream
processors, clear capture buffers, prime playback with `nb_periods*period_size` of silence,
`startDryRunning()` → `syncStartAll()`, retried up to `STREAMPROCESSORMANAGER_SYNCSTART_TRIES`.
Distinct from Linux/bebob: FFADO resyncs the streams in place first; a full teardown (which *does*
re-run CMP) is the fallback when resync retries are exhausted.

---

## 4. Apple `AppleFWAudio.kext` (reverse-engineered, `AppleFWAudio.i64`)

The generic AV/C audio kext (BridgeCo/Oxford/M-Audio/etc.), distinct from Apple's DICE path.
Reverse-engineered from the shipping binary. **This is the closest analog to ASFW's own model**
(one dext, host-side isoch completion callbacks, CoreAudio engine), so its choices are the most
directly portable.

> **DCL / NuDCL is not something ASFW has — do not read it that way.** DCL (DMA Command List) is
> Apple's *portable isoch-program abstraction*; the FWIM compiles a DCL/NuDCL program down into
> the actual OHCI IR/IT descriptor blocks. The `AM824NuDCL*::NuDCLCallback` methods below are just
> Apple's **segment-completion callbacks on that isoch program** — i.e. "a chunk of the isoch DMA
> program finished." ASFW has no DCL layer: it drives OHCI descriptor rings directly. The exact
> equivalents are the **`IsochReceiveContext` (IR) and `IsochTransmitContext` (IT) completions /
> interrupts**. Everywhere §4 says "NuDCL callback," read it as "IR/IT descriptor-block
> completion" for ASFW's purposes — the mechanism is at that level, not at any DCL construct.

### Detection — inter-completion wall-clock watchdog, no registers

The health signal is **"did my isoch-completion callback fire on time,"** measured with
`clock_get_uptime` gap between successive isoch completions — the same "packet cadence = health"
idea as FFADO, sampled at the completion callback instead of the poll loop. (ASFW equivalent: gap
between IR/IT context completions — i.e. *age since last RX packet arrived*.)

- **RX** `AM824NuDCLRead::NuDCLCallback` (`0x4143c`): `diff = now - lastCallback`; threshold
  `0x4C4B400` = **80 ms**. On exceed, `++timesLate` and log
  `"READ callback was late !!!!!! ... timesLate=%lu"` (`0x85b68`).
- **TX** `AM824NuDCLWrite::NuDCLCallback` (`0x4aa50`): threshold `0x2625A00` = **40 ms**
  (device sync) or `0xBEBC200` = **200 ms** when the "Mac Sync" flag (`this+1028`) is set — a
  looser bound when the Mac is clock master. TX **only logs**; it does not escalate.

### Escalation — RX only, debounced, via a fourcc software interrupt

RX is the escalator because its callbacks are driven by the *device's* transmission — a gap there
means the device stopped sending (true timing loss). The debounce is explicit:

```
if (timesLate > 1)                       // 2nd consecutive late RX callback
    device->SendSoftwareInterrupt('late');   // 0x6C617465
```

`AppleFWAudioDevice::SendSoftwareInterrupt` (`0x12dc8`) is a fourcc dispatcher onto an
`IOInterruptEventSource`, which serializes onto the work loop as
`protectedInterruptEventHandler` (`0x13116`). The messages:

| fourcc | value | meaning | handler action |
|--------|-------|---------|----------------|
| `'late'` | `0x6C617465` | RX callback watchdog tripped | **full device reset** (below) |
| `'clok'` | `0x636C6F6B` | loss of clock lock | mute/stop path (`kLossOfClock`) |
| `'srat'`-class | `1936876392` | hardware sample rate changed | reprogram rate, bounce engine |
| `'term'` | `0x7465726D` | device terminating | teardown |

### Recovery — pause → **stop all streams** → settle 1 s → **restart all streams** → resume

The `'late'`/`fDeviceNeedsReset` branch of `protectedInterruptEventHandler` (strings at
`0x67e1c` "Reseting the device!" onward):

1. `pauseAudioEngine`, `IOSleep(100)`
2. `StopAllStreams` (vtable slot 2192), log `"Sleeping to let the streams settle"`, **`IOSleep(1000)`**
3. `StartAllStreams` (slot 2184), `IOSleep(100)`
4. `resumeAudioEngine`; if it had been *running* (state 1), `startAudioEngine`

Every transition is guarded on `!terminating` (`this+3492`) and re-checks engine state — the
teardown/`'term'` race is explicitly handled. The 1 s settle mirrors bebob's warm-up tolerance:
Apple stops, waits a full second for the device/streams to quiesce, then restarts.

There is a **separate** loss-of-clock path (`'clok'`): `IsClockLocked` (`0x135fc`) polls the sync
stream's lock bit (`AM824Read::IsValidSyncStream`, plug-derived — *this* is where a register/plug
read happens, and only for clock, not for stream liveness); `ChangeClockSource` (`0xd93c`) has an
explicit re-lock retry loop (`"clock loss lock retrying retryCount=%lu"`, `"waiting for clock to
lock"`). Clock health and stream health are **separate signals** with separate recovery — do not
conflate them.

### Also present

- `ForceStop` (`AM824DCLReadForceStopNotificationProc` `0x3215c`, `HandleForceStop` `0x12d48`):
  the FWIM/`IOFWIsochChannel` force-stop callback (hardware/bus yanked the channel) → stop that
  side. This is the OHCI-level analog to ASFW's IT/IR FATAL, separate from the cadence watchdog.
  (Apple gets this signal through its DCL/IOFWIsochChannel layer; ASFW gets the same event
  directly from the OHCI IT/IR context — same event, no DCL involved.)
- Bus reset: `busResetAction`/`handleBusResetStart`/`handleBusResetFinish` (`0xa0fe`, `0xa528`,
  `0xa590`) — `handleBusResetFinish` schedules `busResetThreadProc` which re-runs discovery/open;
  the streams themselves recover through the same watchdog/force-stop machinery, not a bespoke
  bus-reset stream fixup. Same "one recovery path" philosophy as bebob.

---

## 5. Synthesis — the three stacks agree

| | Linux (bebob/oxfw) | libffado | Apple AppleFWAudio |
|---|---|---|---|
| **Health signal** | CIP DBC/cycle continuity | time since last packet vs cycle timer | gap between isoch completions (IR/IT in ASFW terms) |
| **Registers read for liveness?** | no | no | **no** (registers only for *clock* lock) |
| **Debounce / tolerance** | JUMBO_PAYLOAD cycle skip | `> 2 s` "distinguish death from xrun" | `> 80 ms` **× 2 consecutive** (RX) |
| **Escalation trigger** | packet error → `-EIO` | handler death / xrun flag | `'late'` fourcc soft-interrupt |
| **Recovery** | domain stop + **CMP break/re-establish** + wait_ready | in-place resync ×N retries, teardown fallback | pause + stop streams + **1 s settle** + restart |
| **Bus reset** | funnels into same xrun path | same | same (+ rediscovery thread) |

**Load-bearing conclusions for ASFW:**

1. **The packet stream is the health signal for AV/C — not a register probe.** All three read
   *cadence/continuity*, never device state, to decide stream liveness. The DICE register probe in
   `DiceAudioBackend` is a DICE-specific luxury (TCAT exposes a clean lock/rate block); it is *not*
   the general AV/C model and must not be assumed portable.
2. **Debounce before escalating.** Every stack tolerates a transient gap (Apple 2×, FFADO 2 s,
   Linux jumbo-cycle). ASFW's `[TxAlign]` self-heal *is* the transient tolerance; the missing piece
   is the escalation *after* the transient window closes.
3. **Escalation = restart the streams with a settle delay**, not a register poke. On the bus this
   re-establishes the connection (bebob breaks CMP explicitly; Apple's stop/start bounces the isoch
   channel and re-arms plugs). A ~1 s settle after restart is expected, not a failure.
4. **Clock lock and stream liveness are distinct.** Apple keeps `'clok'` and `'late'` separate;
   don't fold "device clock unlocked" and "no packets arriving" into one signal.

---

## 6. ASFW design — what the shared coordinator should adopt (FW-64)

**Where:** the timing-loss subscription is duplex-lifecycle logic and belongs in the shared
coordinator / `IsochDuplexHostTransport` path, *not* copied per-backend. Today only
`DiceAudioBackend.cpp:76` calls `SetTimingLossCallback`; `AVCAudioBackend` must gain equivalent
coverage without inheriting the DICE register probe.

**Backend-provided health probe (interface, not implementation):** give `IAudioBackend` a
`ProbeStreamHealth()` the coordinator calls when timing-loss fires. Backends implement it with
whatever ground truth they have:

- **DICE**: existing register probe (GLOBAL/STATUS/EXT_STATUS lock+rate) — keep as-is.
- **AV/C**: **no registers.** Use the reference-validated cadence signal ASFW already computes in
  its **`IsochReceiveContext` (IR)** path — RX replay-established + "packets seen recently." The IR
  interrupt already captures a host-uptime timestamp (the ZTS anchor is taken there), so the probe
  is simply *age since the last IR completion / last RX packet*, not any new callback. (Apple gets
  the same number from its NuDCL segment-completion gap; ASFW gets it from the IR context — same
  signal, no DCL layer.) Healthy-but-stalled (age small) → let `[TxAlign]` self-heal; stalled (age
  past a debounce ≈ Apple's 80 ms × 2, or ≥ one IO window) → escalate.

**Escalation action:** coordinator restart that re-primes the streams (our analog to Apple's
stop→settle→start and bebob's CMP break/re-establish). The existing DICE restart-session FSM
(`DICERestartSession`, `RestartJournal`) is the vehicle; AV/C needs the same terminal action wired
to its health verdict.

**Two concrete fixes already identified (from §1):**

- Wire AV/C timing-loss → health probe → escalation (this section).
- `frameShort` branch of `[TxPrepRange]` (`ASFWAudioDriverZts.cpp:640`): make it
  transition-only / rate-limited so a persistent stall logs once, not at ~1 kHz.

---

## 7. FCP interaction (FW-100 — FCP attempt state)

FCP is the **control** channel (AV/C command/response over the FCP register space); stream health
above is the **data** channel. They intersect at bus reset, and getting the ordering wrong is a
classic AV/C hang.

- **Bus reset invalidates in-flight FCP transactions.** Linux funnels all of it through
  `fcp_bus_reset()` (`bebob.c:326`, called from `.update`): the bebob bus-reset handler does
  *nothing* to the streams (the DBC discontinuity handles those via XRUN) and *only* resets FCP
  state. Mirror this split in ASFW — the FCP attempt-state machine (FW-100) is reset on bus reset
  independently of stream recovery; do not drive stream restart from the FCP path or vice-versa.
- **Do not issue FCP during the reset window.** An AV/C command whose response is lost to the reset
  must be re-attempted against the new generation, not left pending — this is exactly what FW-100's
  attempt-state tracks. libffado/Linux both treat a reset-straddling FCP transaction as failed and
  retry on the new generation.
- **Recovery may itself require FCP.** bebob's `start_duplex` re-runs rate get/set and
  format/connection commands (`bebob_stream.c:537-590`) — i.e. escalation issues fresh FCP after
  the streams are torn down. So the ordering during a coordinator restart is: quiesce streams →
  (bus generation stable) → FCP reconfigure (rate/format/CMP) → restart streams → settle. FCP must
  be *quiescent and re-attemptable*, never in-flight, across the restart boundary.
- **CMP (connection management, `oPCR`/`iPCR`) is FCP-adjacent but wire-observable.** Breaking and
  re-establishing a connection is what makes a restart visible on the bus (§2, §4). ASFW's AV/C
  escalation should re-establish the plug connection as part of the restart, matching bebob's
  `break_both_connections` + `cmp_connection_establish`.

**Net:** keep three state machines separate but ordered — bus-generation, FCP attempt-state
(FW-100), and stream health/restart (FW-64) — with escalation running FCP reconfigure *between*
stream teardown and stream restart, never concurrently with an in-flight FCP command.