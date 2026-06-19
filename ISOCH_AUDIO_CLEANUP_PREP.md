# ISOCH_AUDIO_CLEANUP_PREP.md — Removing Audio Knowledge from the Isoch Stack

**Status:** prep plan, drafted 2026-06-11. Companion to `ISOCH_AUDIO_ADK.md`.
That document says where we're going; this one says what must be torn out of the
current tree *first* so the new boundary starts clean — no double paths, no
"old path kept around just in case" rotting next to the new one.

**The litmus test for every file below:** after cleanup, the core driver must
compile without knowing what a sample, a channel, a CIP header, a DBC, or an SYT
is. The refill ISR's entire AMDTP knowledge is "copy 8 bytes and a length"
(ADK doc §4.3). Anything in `Isoch/` that fails this test either moves to the
audio side, gets reshaped, or dies.

---

## 1. Why delete before building (the double-path question)

The current direct TX path (`IsochAudioTxPipeline` + `TxOutputPhaseLoop` +
provider/preparer callbacks) is a **synchronous pull model**: the core's refill
calls *into* audio code (`NextTransmitPacket`, `PreparePayload`) on the core
queue. The new design is a **shared-memory push model**: the audio side commits
packets into a metadata ring ahead of time, and the ISR only blits. These two
models cannot share the `IsochTxDmaRing` interface — the callback interfaces
*are* the boundary violation. Building the new path "alongside" the old one
means every DMA-ring function grows an `if (legacyPath)` branch. That is the
dirty code we're avoiding.

**Cost we accept:** the bench goes TX-dark between teardown (Stage 2) and ADK
Phase 3 (connect). This is cheaper than it sounds:

- Current TX plays real audio since commit `79e45e92` (ring capacity synced to
  the 512-frame ZTS period — that closed the silence bug in `DICE_DEBUG.md`),
  **but with underruns**: the fix pins a 384-frame consumer lead inside a
  512-frame ring, leaving ~128 frames (~2.7 ms) of writer headroom. That
  tightness is structural — ring length *must* equal the ZTS period in the old
  path (wrap-mismatch class, commits `0d897ecb`/`79e45e92`), so headroom cannot
  be widened without reintroducing the silence bug. The new design dissolves
  the conflict: slab depth is sized independently of the ZTS period and of the
  lead (§6.4), and HAL-side underruns degrade to pre-zeroed silence (§6.1). Do
  not spend bench time tuning the old path's 384/64 constants.
- Protocol correctness regression coverage lives in `ADKVirtualAudioLab/`
  (19.5-min soak, wire-parity with the Saffire capture), not in the bench path.
- RX stays alive (§6 below), so bus bring-up, DICE handshake, and clock
  observation remain testable on hardware throughout.

**What we must NOT lose:** the empirical knowledge embedded in the old path —
the SYT graft (`0x?B0` sub-cycle), the dead ends already explored, the
two-clock doctrine. All of it is already captured in `ISOCH_AUDIO_ADK.md` §5
and memory notes. The *code* can die; the *findings* are preserved. Stage 0
guarantees the code itself stays recoverable from git.

---

## 2. Inventory: DELETE (superseded by the lab stack + new design)

### 2.1 The direct TX pipeline (`AudioEngine/DirectIsoch/`, entire directory)

| File | Why it dies |
|---|---|
| `IsochAudioTxPipeline.{hpp,cpp}` (1288 lines) | The old audio-semantics layer the core context *embeds by value* (`IsochTransmitContext::audio_`). Replaced by the lab pump + payload writer running in the audio dext. Its `TxPcmPacketRing` is a second PCM staging copy — violates the one-transform-pass floor (ADK §6.5). |
| `Timing/TxOutputPhaseLoop.{hpp,cpp}` | Per-packet phase PLL against the device's recovered clock. Superseded by deterministic cycleMatch start (ADK §5.2) + the settled SYT recipe (§5.4). |
| `Timing/OutputPhaseToAudioMap.hpp` | Mapping layer for the phase loop; dies with it. |
| `Sync/ExternalSyncBridge.hpp` | RX→TX SYT/phase side-channel feeding the phase loop. The new TX derives SYT from its own transmit cycle, not from RX state. The future RX→TX input is the FW-26 SYT-delta cadence ring fed by the RX metadata ring (ADK §7) — a different structure at a different boundary. |
| `Sync/ExternalSyncDiscipline48k.hpp` | Dies with the bridge. |

Deleting the bridge touches `IsochReceiveContext` (it publishes into the
bridge) and `IsochService` (owns `externalSyncBridge_`): remove the publish
calls and the member. Light edits, not an RX redesign.

### 2.2 The direct TX encoders/processors (`AudioEngine/Direct/Tx/`, entire directory)

`TxAudioPacketProcessor.{hpp,cpp}`, `TxAudioPacketWriter.{hpp,cpp}`,
`DirectTxPacketEncoder.hpp`, `DirectTxPacketScratch.hpp`, `DirectTxTypes.hpp`,
`DirectTxProbe.{hpp,cpp}`, `OutputCursorDiscipline.hpp`.

All of this is the old path's float→wire stage and its cursor/lead heuristics
(`txOutputOffsetFrames_` stopgap territory). The lab's payload writer +
`RawPcm24In32Encoder` is the replacement, running on the RT thread in the
audio dext (ADK §4.2).

### 2.3 TX support machinery in `Audio/Runtime/`

| File | Verdict |
|---|---|
| `TxPacketState.hpp` | Delete — old-path slot state. New slot state is `TxPacketMeta.commitGen` in the shared header. |
| `TimingCursorPolicy.hpp` | Delete — old-path lead/deadband policy (`MakeDice48kBlocking`). New lead policy = coverage target in the pump (ADK §4.1) + the Saffire `delayPackets` table (§6.6). |
| `PlaybackRingRange.hpp`, `AudioSampleRing.hpp`, `ZtsAuthority.hpp`, `ZtsTimelineCalculator.hpp` | Audit at Stage 2: delete what only the old TX path references; what the ADK ZTS path (`ASFWAudioDriverZts.cpp`) genuinely uses stays on the audio side. |

### 2.4 Core-side verification & recovery (`Isoch/Transmit/`)

| File | Why it dies |
|---|---|
| `IsochTxVerifier.{hpp,cpp}` (588 lines), `TxVerifierDecode.hpp` | The verifier **decodes CIP in the core** — the definition of the leak. Verification belongs to the lab (byte-exact CIP verifier, soak gates) and to audio-side telemetry. |
| `TxPayloadHash.hpp` + the `preparedPayloadHash`/`completedPayloadHash` plumbing | New rule: *the core CPU never reads or writes payload bytes* (ADK §3.1). Hashing payloads in the core is now a forbidden access pattern, not just dead code. |
| `SimITEngine.hpp` | Host-test IT simulator for the pull model. The new contract is rehearsed in the lab with a fake "core ISR" consuming the metadata ring (ADK Phase 1) — that fake replaces this one. |
| `IsochTxRecoveryController.{hpp,cpp}` | Old restart gating. Replaced by the §6.2/§6.3 policy: core stops the context, sets `statusWord`, bumps `streamGeneration`; the *audio side* owns re-negotiation. No recovery orchestration in the core. |

Their hooks die with them: `KickTxVerifier()` / `ServiceTxRecovery()` calls in
`WatchdogCoordinator.cpp:152-153`, the `RecoveryCallback` /
`TxRecoveryCallback` plumbing through `IsochService` and `ASFWDriver.cpp`, and
the `IsochTxCaptureHook` / `IIsochTxAudioInjector` / `IIsochTxCompletionObserver`
interfaces in `IsochTxDmaRing.hpp`.

### 2.5 Audio vocabulary baked into core types

| Item | Where | Why it dies |
|---|---|---|
| `IsochTxPacket.{dbc, syt, framesPerPacket, audioFrame, outputPhaseTicks, isData}` | `IsochTxDmaRing.hpp:23-32` | The ISR must not be able to tell DATA from NO-DATA (ADK §3.2). Replacement type: `{immediateHeader[2], payloadLength}` read from the metadata ring. |
| `PreparedTxSlotMetadata` (dbc, syt, audioFrame, firstSourceSamples, encoded words…) | `IsochTxDmaRing.hpp:67-82` | Whole per-slot audio metadata mirror in the core. The metadata ring entry replaces it; completion side shrinks to `{packetIndex, xferStatusTimestamp}` → `completionStamps`. |
| `IIsochTxPacketProvider`, `IIsochTxPayloadPreparer` | `IsochTxDmaRing.hpp` | The synchronous pull interfaces. The metadata ring *is* their replacement. |
| `DataPackets()` / `NoDataPackets()` counters | `IsochTransmitContext.hpp:117-118` | Core counting DATA vs NO-DATA = core parsing CIP. Cadence telemetry moves to the pump. |
| `RequestedStreamMode()` / `EffectiveStreamMode()`, `Encoding::AudioWireFormat` params | `IsochTransmitContext`, `IsochService` | Blocking/non-blocking and wire format are protocol decisions; core sees only `maxPacketBytes`. |
| `SampleRate`, `SampleRateFamily`, `GetFamily()` (IEC 61883-6) | `Isoch/Core/IsochTypes.hpp` | Move to `AudioWire/AMDTP/` (it's CIP SFC vocabulary). Core keeps only context-level types. |
| `DumpPayloadBuffers()` | `IsochTransmitContext`, `IsochTxDmaRing` | Core reading payload bytes, even for debug, violates §3.1. Payload dumps become an audio-side / lab facility. |

### 2.6 Tuning-profile configs (`Isoch/Config/`)

| File | Verdict |
|---|---|
| `AudioTxProfiles.hpp` | Delete. The A/B/C `startWaitTargetFrames`/`safetyOffsetFrames` guessing ladder is superseded by the RE'd Saffire model: safety offset = `delayPackets × framesPerPacket` from the fixed `latencyMode × rate` table (ADK §6.6). |
| `AudioRxProfiles.hpp` | Lives until Stage 5 (RX still uses it), then dies the same way. |
| `AudioConstants.hpp` | Split: pure wire caps (`kMaxAmdtpDbs`, `kMaxPcmChannels`) → `AudioWire/`; ring/period sizing (`kAudioRingBufferFrames`, `kAudioIoPeriodFrames`, lead/deadband constants) → the Phase 0 shared header where both sides `static_assert` them (ADK §3.3 iron rule). Nothing stays in `Isoch/Config/`. |
| `AudioConfig.hpp` | Umbrella include — dies when its children do. |
| `Isoch/Encoding/` | Empty directory. Remove. |

---

## 3. Inventory: MOVE (code survives, ownership changes)

These are correct implementations that live on the wrong side of the boundary
or need an ownership rule rather than a rewrite:

| What | From → To | Note |
|---|---|---|
| `AudioWire/` (AM824, AMDTP, CIP, RawPcm24In32) | stays in place; **include rule changes** | Pure protocol library, already validated via the lab. New rule: only audio-dext files and the lab may include it. `grep -rl "AudioWire" ASFWDriver/Isoch/` must return empty after Stage 3 — make this a CI/gate grep. |
| `SYTGenerator`, `TimingUtils`, `BlockingCadence48k`, `PacketAssembler` | already in `AudioWire/AMDTP/` | Consumed by the pump in the audio dext from now on. The SYT graft recipe (§5.4) is the part of the old pipeline worth keeping — it's already here, not in the deleted files. |
| `SampleRate` / `SampleRateFamily` enums | `Isoch/Core/IsochTypes.hpp` → `AudioWire/AMDTP/AmdtpRateGeometry.hpp` (or sibling) | See §2.5. |
| `AudioClockPublisher`, `DirectOutputReader`, `DirectInputWriter` (`AudioEngine/Direct/`) | TX usage dies at Stage 2; RX usage frozen until Stage 5 | The ZTS publication role is rebuilt on completion stamps + clock pairs (ADK §5.5) inside `ASFWAudioDriverZts.cpp`. |
| `AudioTransportControlBlock` (`Audio/DriverKit/Runtime/`) | evolves **in place** into control block v2 | One control block, versioned — do not create a second one next to the old one. v2 adds `streamGeneration`, `clockPair`, `startAnchor`, `completionCursor/Stamps`, `exposeCursor` (ADK §3.3) and drops old-path fields. |

---

## 4. Inventory: RESHAPE (core keeps the file, audio vocabulary comes out)

| File | Keeps | Loses |
|---|---|---|
| `IsochService.{hpp,cpp}` | IRM reservation (`ReservePlayback/CaptureResources` — `bandwidthUnits` is bus-level), context lifecycle, GUID claim, generation/teardown signaling | `IsochDuplexStartParams` audio fields (`sampleRateHz`, `host*PcmChannels`, `*Am824Slots`, `*WireFormat`, `streamMode`, `directAudioBindingSource`), `externalSyncBridge_`. New surface = the nub API: `AllocateTxIsochResources(numSlots, maxPacketBytes, interruptInterval, …)` / `StartTxStream(channel, speed)` (ADK §7). |
| `IsochTransmitContext.{hpp,cpp}` | Context state machine, channel/contextIndex, interrupt + watchdog entry points, refill latency histogram, `WakeHardware` | `IsochAudioTxPipeline audio_` member, verifier/recovery members, binding-source/sync-bridge setters, stream-mode accessors, DATA/NO-DATA counters, payload dumps. Becomes: descriptor-ring lifecycle + refill ISR consuming the metadata ring + completion-stamp publishing. |
| `IsochTxDmaRing.{hpp,cpp}` | Descriptor slab management, cycle tracking & resync, hw packet-index decode, gap counters, `WakeHardwareIfIdle`, OMI/OL patching mechanics | All §2.5 types and callbacks. Refill loop changes from "call provider, copy payload" to "acquire-load `commitGen`, blit `immediateHeader` + `reqCount`, fix branch word" — and the slab's payload pointers are programmed once at allocation, never repointed (ADK §3.1). |
| `IsochTxLayout.hpp` | OHCI page-padding math (`kOHCIPageSize`, prefetch, descriptors-per-page), 3-blocks-per-packet shape | Hard-coded stream sizing (`kNumPackets=192`, `kTimingGroupPackets=8` "Saffire DCL group", `kAudioWriteAhead`, `kGuardBandPackets`) — these become the negotiated `numSlots` / `interruptInterval` parameters with the §6.4 sizing rule (128–256 slots, depth ≠ latency). |
| `WatchdogCoordinator.cpp` | Isoch tick/poll, hw-state logging | `ServiceTxRecovery()` / `KickTxVerifier()` calls. |
| `UserClient/Handlers/IsochHandler.cpp` | RX debug surface (until Stage 5) | TX dump/verifier entry points (die with their targets). |

**Keep untouched (already on the right side of the line):**
`IsochTxDescriptorSlab.{hpp,cpp}`, `Memory/IsochDMAMemoryManager.{hpp,cpp}` +
`IIsochDMAMemory.hpp`, `Core/IsochEventGroup.hpp`, `InterruptDispatcher`
routing, all of `Hardware/`.

---

## 5. Consumers to rewire (the call-site sweep)

Found by grep, so this list is the actual blast radius, not a guess:

- `ASFWDriver.cpp` / `ASFWDriver.iig` / `Service/DriverContext.hpp` — owns
  `IsochService`, wires recovery/timing-loss callbacks → replace with the
  generation/status discipline.
- `Audio/Core/AudioCoordinator.{hpp,cpp}` — orchestrates duplex start with the
  audio-vocabulary params → re-target to the new nub allocate/start API.
- `Protocols/Audio/Backends/{AVCAudioBackend, DiceAudioBackend, DiceHostTransport}`
  and `DiceDuplexRestartCoordinator` — same re-targeting; the DICE bring-up
  controller keeps doing device-side setup (rates, router, EXT_STATUS) — that
  knowledge is *device* protocol, not OHCI, and stays where it is.
- `Bus/BusResetCoordinator.cpp` — currently pokes contexts; becomes "bump
  `streamGeneration`, stop contexts" (ADK §6.3).
- `Hardware/InterruptDispatcher.cpp` — unchanged shape (routes isoXmit/isoRecv
  events to contexts), just loses dead parameters.
- `UserClient/Handlers/IsochHandler.cpp` — TX entry points pruned (§4).
- `tests/` — suites covering deleted components (verifier, recovery controller,
  pipeline, phase loop) are deleted with them or ported to the lab where the
  behavior they guarded now lives. Do not keep tests alive for dead code.

---

## 6. RX: frozen, not cleaned (deliberate exception)

The RX direct path (`IsochReceiveContext` + `IsochRxDmaRing` +
`AudioEngine/Direct/Rx/` + `DirectInputWriter` + RX side of
`AudioClockPublisher` + `AudioRxProfiles.hpp`) has the **same** boundary
violations as TX. It is *not* in scope until ADK Phase 5, because:

1. It works on the bench and is the only hardware-facing monitor we have while
   TX is dark.
2. The Saffire ZTS model anchors on IR — we want the working RX behavior
   observable right up until its replacement (RX metadata ring with
   `{actualLength, descriptorTimestamp}`) is ready to A/B against it.

Ring-fence rules until then: no new code may depend on the RX direct path; no
new fields in its types; the ExternalSyncBridge publish calls come out at
Stage 2 (the TX consumer is gone). Its deletion list mirrors §2 one-for-one
and should be executed as the first step of ADK Phase 5.

---

## 7. Device profiles: how the audio dext learns what to stream

*(Designed here 2026-06-11 — this mechanism is missing from `ISOCH_AUDIO_ADK.md`,
whose nub API takes `maxPacketBytes`, `channel`, `speed` as if already known.
This section is the answer to "known by whom, from where".)*

### 7.1 The problem

The pump cannot emit a single packet without device-specific facts: dbs, the
audio/MIDI slot split, wire format (AM824 `0x40`-labeled vs sign-extended
Raw24-in-32 — the bench Saffire uses *different formats per direction*),
blocking mode, FDF, iso channels, the SYT sub-cycle graft, and the
`delayPackets` latency table. Today this knowledge is smeared across three
half-mechanisms, none of which crosses the boundary completely:

1. `DeviceProfiles/` (refactor Phase A) — identity + family/integration hints
   only; no stream geometry.
2. `Protocols/Audio/DICE/TCAT/DICEKnownProfiles.hpp` — a **second** known-device
   table (self-described as "temporary"), holding channel/slot counts for
   Focusrite/Alesis devices outside the `DeviceProfiles/` registry. This is the
   duplicate path to kill.
3. `ASFWAudioNub_IVars` — delivers a 5-field subset (channel counts, rate,
   `streamModeRaw`) to the audio dext. No wire format, no dbs/MIDI split, no
   latency model, no SYT discipline.

### 7.2 The design: one document, three sources, one delivery point

**The document.** A POD `AudioStreamProfile` in the Phase 0 shared header
(next to `TxPacketMeta` / control block v2, same `static_assert` regime),
containing per-direction `StreamDescriptor`s plus device-level policy:

```c
struct StreamDescriptor {              // one per direction
    uint8_t  wireFormat;               // kAM824Labeled | kRaw24In32SignExtended
    uint8_t  dbs;                      // quadlets per frame on the wire
    uint8_t  pcmSlots, midiSlots;      // dbs = pcmSlots + midiSlots
    uint8_t  blockingMode, fdf;
    uint8_t  isoChannel, speed;
};
struct AudioStreamProfile {
    StreamDescriptor hostToDevice, deviceToHost;
    uint32_t sampleRateHz;
    uint8_t  framesPerPacket;          // rate ladder (8/16/32, ADK §6.6)
    uint8_t  sytMode;                  // kConstantSubCycleGraft | kFreeRunning…
    uint8_t  latencyMode;              // index into the delayPackets table
    uint8_t  inputDelayPackets, outputDelayPackets;   // resolved table row
    uint16_t reportedLatencyBaseFrames;
    uint32_t profileGeneration;        // see 7.4
};
```

The audio dext *derives* everything the nub API needs from this:
`maxPacketBytes = 8 + framesPerPacket × dbs × 4`, `numSlots` from the §6.4
sizing rule, CoreAudio formats from `pcmSlots`/rate, safety offset from
`delayPackets × framesPerPacket`. The core never interprets the audio fields —
it allocates what it's asked to allocate. The boundary proof carries over: the
profile flows core → audio as *data*; no protocol code crosses.

**The three sources, merged by the core-side resolver in precedence order:**

1. **Discovered** (highest) — what the device actually says: DICE register
   decode (rate, stream-config entries → dbs/channels/iso channels,
   EXT_STATUS), AV/C ExtendedStreamFormat for that family. Authoritative
   because wire = truth.
2. **Known-device table** — `DeviceProfiles/Audio/Vendors/*` grows a
   `LookupStreamProfile(query)` provider alongside the existing identity/hint
   lookups. The contents of `DICEKnownProfiles.hpp` move here (Focusrite,
   Alesis rows) and that file is deleted — one registry, one mechanism. This
   tier fills what discovery can't see: wire-format quirks (sign-extension!),
   SYT graft mode, latency tables, and full geometry for devices whose
   discovery is unreliable.
3. **Family defaults** (lowest) — generic AMDTP/IEC 61883-6 assumptions keyed
   off the existing `AudioProtocolFamily` hint, so an unknown-but-conformant
   device still gets a sane profile.

**The resolver** lives core-side next to `AudioRuntimeRegistry` in the
controller dependencies (the ownership home established by the completed
DeviceProfiles Phase B refactor, commit `e8948d3` — controller/deps-owned
because that's where bus + IRM + `DeviceRegistry` already live at the
discovery trigger site). `AudioStreamRuntimeCaps` survives as the *output of
tier 1 discovery* feeding the resolver — it stops being the thing handed to
audio.

**The delivery point.** One new LOCALONLY nub method (same-process plain call,
setup-time only, consistent with ADK §7):

```cpp
kern_return_t CopyAudioStreamProfile(AudioStreamProfile* outProfile);
```

replacing the ad-hoc `ASFWAudioNub_IVars` caps subset. The audio dext reads it
at match time and at every generation bump, then calls
`AllocateTxIsochResources` / `StartTxStream` with values derived from it.

### 7.3 Config changes flow the other way (decision vs execution)

When CoreAudio asks for a new sample rate (or the user picks a clock source),
the *decision* is the audio dext's, but the *execution* is device protocol —
core territory. One setup-time nub method:

```cpp
kern_return_t RequestDeviceConfiguration(uint32_t sampleRateHz, uint32_t clockSourceId);
```

Core's protocol backend (DICE/AVC) performs the register writes, re-runs the
resolver, republishes the profile, and bumps the generation. The audio dext
never touches device registers; the core never decides audio policy.

### 7.4 Generation discipline (one mechanism, not two)

The profile does **not** get its own invalidation channel.
`profileGeneration` is published alongside — and bumped together with — the
control block's `streamGeneration` (ADK §6.3): bus reset, rate change, dead
context, teardown all funnel through the same path. Audio-dext touchpoints
already re-check `streamGeneration`; on mismatch the re-negotiation sequence
becomes: re-read profile → re-derive geometry → re-allocate → restart. No
second watchdog, no profile-change callback.

### 7.5 Cleanup consequences (added to the inventories above)

| Item | Verdict |
|---|---|
| `Protocols/Audio/DICE/TCAT/DICEKnownProfiles.hpp` | **Delete** — rows move into `DeviceProfiles/Audio/Vendors/{Focusrite,Alesis}AudioProfiles.hpp` as `LookupStreamProfile` providers. |
| `Protocols/Audio/AudioTypes.hpp` (`AudioStreamRuntimeCaps`) | **Reshape** — becomes the tier-1 discovery output consumed only by the resolver; no longer crosses to the audio side. |
| `ASFWAudioNub_IVars` caps fields (`channelCount`, `inputChannelCount`, `outputChannelCount`, `currentSampleRateHz`, `streamModeRaw`) | **Delete** — replaced by `CopyAudioStreamProfile`. |
| New grep gate | Per-device constants (vendor/model IDs, slot counts, latency rows) may exist **only** under `DeviceProfiles/` — `grep -rnE "0x00130e|vendorId ==" ASFWDriver/Protocols/` must not match device tables after Stage 4. |

Stage placement: the `AudioStreamProfile` struct lands with the shared header
(Stage 4 = ADK Phase 0); the resolver + `CopyAudioStreamProfile` +
`DICEKnownProfiles` fold land with the core API reshape (Stage 3 = ADK
Phase 2/3 boundary, since Phase 3's connect step is its first real consumer).

---

## 8. Execution stages (each gate is a hard stop)

Stages align with `ISOCH_AUDIO_ADK.md` §9 phases — the cleanup is the leading
edge of the same work, not a separate project.

**Stage 0 — preserve state.** Commit the current working tree (there is an
uncommitted nub diff from the direct-binding fix — land it or stash it
deliberately, don't let the sweep eat it). Tag `pre-isoch-cleanup` so the old
path is one checkout away forever.
*Gate: clean `git status`; tag pushed.*

**Stage 1 — dead weight (no behavior change).** Delete §2.4 (verifier, decode,
payload hash, sim engine, recovery controller) + their watchdog/user-client
hooks, the empty `Isoch/Encoding/`, and `DumpPayloadBuffers`. These are
dev-only facilities whose successor (the lab) already exists.
*Gate: `./build.sh` green, `./build.sh --test-only` green after pruning the
matching test suites.*

**Stage 2 — TX path teardown (bench goes TX-dark).** Delete §2.1, §2.2, §2.3;
strip §2.5 types down to their replacements; prune `IsochService` /
`IsochTransmitContext` / call-site surface (§4, §5); remove ExternalSyncBridge
publish calls from RX. RX must still run.
*Gate: build + tests green; bench check: RX capture and DICE bring-up still
work; grep gate `grep -rlE "AM824|CIPHeader|SYTGen|PacketAssembler" ASFWDriver/Isoch/`
returns empty.*

**Stage 3 — core API reshape (= ADK Phase 2 start).** `IsochTxLayout` sizing →
negotiated params; `IsochTxDmaRing` refill → metadata-ring consumer;
`IsochService` → `AllocateTxIsochResources` family on the nub; profile
resolver + `CopyAudioStreamProfile` on the nub; fold `DICEKnownProfiles.hpp`
into the `DeviceProfiles/` vendor providers (§7). This stage *is* the
beginning of new-path construction; the cleanup ends where ADK Phase 2
begins.
*Gate: ADK Phase 2 gate (clean fixed-cadence stream from a trivial NO-DATA
generator, watched with FireBug).*

**Stage 4 — constants consolidation (= ADK Phase 0, can run before/parallel to
Stage 3).** `AudioConstants.hpp` split per §2.6; `AudioTransportControlBlock`
→ v2 in the shared header; `AudioStreamProfile` struct in the shared header
(§7.2); `static_assert`s on sizes/offsets both sides.
*Gate: both targets + lab compile against the one shared header; lab tests
green.*

**Stage 5 — RX mirror teardown (= ADK Phase 5, deferred).** Execute §6's
mirror list when the RX metadata ring lands.

---

## 9. Grep gates (cheap permanent guards)

After Stage 3, these must all return empty — wire them into review habit or CI:

```sh
# Core must not include the protocol library
grep -rl "AudioWire" ASFWDriver/Isoch/
# Core must not speak AMDTP vocabulary (RX exempt until Stage 5)
grep -rlE "syt|dbc|am824|AM824|cadence|sampleRate" ASFWDriver/Isoch/Transmit/
# Nobody but the audio dext touches payload bytes
grep -rl "PayloadForAudioFrame\|CopyPacketPayload" ASFWDriver/Isoch/
```

---

*Related: `ISOCH_AUDIO_ADK.md` (target design), `ADKVirtualAudioLab/README.md`
(protocol regression coverage that makes the deletion safe), `DICE_DEBUG.md`
(history of the TX silence bug — resolved by `79e45e92`; the old path's
remaining defect is the structural underrun headroom described in §1).*
