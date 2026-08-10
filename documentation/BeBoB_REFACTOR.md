# BeBoB Backend Refactoring Plan

## Goal

Separate the current `Phase88Protocol` monolith (556 lines) into:
- **Large, general BeBoB protocol** — reusable for any BridgeCo BeBoB device
- **Small Phase88 overlay** — only device-specific mixer config, geometry, identity
- A `GenericBeBoBProtocol` for known-but-untested devices (discovery only)

## Constraints

| Constraint | Source |
|------------|--------|
| No 1:1 copying from Linux bebob (GPLv2) or libffado (GPL) | Project doctrine + user |
| Fresh C++23 + DriverKit patterns (RAII, `std::expected`, `std::span`, `constexpr`) | User |
| Apache 2.0 preserved on all new files | User |
| No `IOSleep()` — use `Scheduling::ITimerScheduler` for genuine async delays | Reviewer correction |
| Wire behavior cross-validated against references, cited by `file:line` | Project doctrine |
| Only Phase88 verified; other devices at `kDiscoveryOnly` support level | User suggestion + reviewer |
| No vendor-ID-range matching — exact vendor/model only | Reviewer correction |
| Identity metadata lives in existing profile layer, not a new table | Reviewer correction |

---

## Architecture After Refactoring

```
ASFWDriver/Audio/Protocols/BeBoB/
├── BeBoBProtocol.hpp/.cpp             ← NEW: abstract BeBoB base class (~350 lines)
├── BeBoBMixerMap.hpp                  ← NEW: declarative mixer description types
├── BeBoBPlug0StreamDiscovery.hpp/.cpp ← MOVED + RENAMED from BridgeCoReadOnlyProbe (plug-0 scope)
├── GenericBeBoBProtocol.hpp/.cpp      ← NEW: generic fallback for known-but-untested devices
├── Phase88Protocol.hpp/.cpp           ← SHRUNK: Phase88 overrides only (~120 lines)
└── Phase88MixerData.hpp               ← NEW: Phase88 static mixer map data

ASFWDriver/Audio/DriverKit/Config/AVC/
├── Phase88Profile.hpp/.cpp            ← unchanged (static ADK geometry)
└── BeBoBProfile.hpp/.cpp              ← NEW: per-GUID discovery-derived ADK geometry

ASFWDriver/DeviceProfiles/Audio/Vendors/
├── TerraTecAudioProfiles.hpp         ← generalized to BeBoB hook
└── BeBoBDeviceProfiles.hpp           ← NEW: BeBoB identity + profile dispatch + support levels

Protocols/AVC/
├── AudioFunctionBlockCommand.hpp      ← unchanged (stays as AV/C transport primitive)
├── AVCUnitPlugSignalFormatCommand    ← unchanged (stays as AV/C transport primitive)
├── AVCDiscovery.cpp                   ← CHANGED: IsTerraTecPhase88 → IsBeBoBDevice
└── BridgeCo/                          ← DELETED (moved to Audio/Protocols/BeBoB/)

Audio/Protocols/Backends/
├── DuplexStreamProfile.hpp            ← CHANGED: IsTerraTecPhase88 → IsBeBoBDevice
└── (AVCAudioBackend unchanged)
```

---

## Detailed Design

### `BeBoBProtocol` — Abstract Base Class

**Namespace:** `ASFW::Audio::BeBoB`
**Implements:** `IDeviceProtocol`, `IDuplexDeviceControl`, `IAVCCommandSubmitter`
**Constructor requires:**
- `OSSharedPtr<IODispatchQueue> workQueue` — for FCP dispatch
- `Scheduling::ITimerScheduler& timerScheduler` — for genuine async delays (NOT `IOSleep`)

**Contains (extracted from current Phase88Protocol):**
- CMP lifecycle: `ProgramRx`, `ProgramTxAndEnableDuplex`, `DisconnectPlayback/Capture`, `BreakBothConnections`, `StopDuplex`, `EnsurePlugFree`
- Signal format programming (OUTPUT→INPUT plug 0, AM824) — but plug and rate are policy hooks
- Stream lifecycle: `PrepareDuplex`, `ConfirmDuplexStart`, `UpdateRuntimeContext`, `ResetEpochIfNeeded`
- FCP transport dispatch (`SubmitCommand`)
- Safe completion capture (no `this` in CMP callbacks)

**`ApplyClockConfig` async algorithm:**
```
ProgramSignalFormat → ConfigureMixer(callback) [virtual, async] → ITimerScheduler(settle) → callback
```

Every continuation checks an epoch token; `Finish()` guarantees exactly one completion. `Shutdown()` cancels the timer and invalidates the epoch.

**Virtual hooks:**
| Hook | Default | Purpose |
|------|---------|---------|
| `ConfigureMixer(MixerCompletion)` | calls completion immediately | Async FB mixer program |
| `DeviceCaps()` | pure virtual | Per-device channel geometry |
| `DeviceName()` | pure virtual | Human-readable name |
| `SupportedRates()` | pure virtual | Per-device rate list |
| `StreamPlugs()` | returns `{0, 0}` | Which plugs to connect |
| `BuildSignalFormat(rate, plug)` | pure virtual | Signal format for plug/rate |
| `ReadClockHealth(callback)` | pure virtual | Clock health policy |

**Mixer failure policy** (async, distinguishes required vs best-effort):
```cpp
enum class MixerFailurePolicy {
    kRequired,    // abort clock apply on failure
    kBestEffort,  // log and continue (current Phase88 behavior)
};

virtual void ConfigureMixer(MixerFailurePolicy policy, MixerCompletion completion);
```

**FB framework helpers (hide `ControlSelector` shared-enum sharp edge):**
- `SetSelectorBlock(fbId, value, completion)`
- `SetFeatureMute(fbId, channel, mute, completion)`
- `SetFeatureVolume(fbId, channel, value, completion)`

All are async (take completion callbacks) since they submit FCP commands.

**Cancellation and exactly-once completion:**
```cpp
struct ClockApplyEpoch {
    uint64_t generation;          // invalidated on bus reset
    std::atomic<bool> completed;  // Finish() CAS guard
    TimerToken settleTimer;       // ITimerScheduler handle
    ClockApplyCallback completion;
};
```

---

### `GenericBeBoBProtocol` — Concrete Fallback

**For known-but-untested devices** (support level `kDiscoveryOnly`).

Inherits `BeBoBProtocol`, provides:
- `DeviceCaps()` from discovery results (`DeviceModel`)
- `DeviceName()` from identity table
- `SupportedRates()` from stream formation scan
- `StreamPlugs()` = `{0, 0}` (conservative; one CMP stream)
- `BuildSignalFormat()` = AM824 at the negotiated rate
- `ConfigureMixer()` = calls completion immediately (no mixer programming)
- `ReadClockHealth()` = PCR connectivity check

This makes the support boundary unmistakable: verified devices get a custom protocol; unknown devices get generic streaming with conservative defaults.

---

### `BeBoBMixerMap.hpp` — Declarative Mixer Types

```cpp
namespace ASFW::Audio::BeBoB {

struct SelectorRoute { uint8_t fbId; uint8_t value; };
struct ChannelMute  { uint8_t fbId; uint8_t channel; bool unmute; };
struct ChannelVolume { uint8_t fbId; uint8_t channel; uint16_t value; };

struct MixerMap {
    std::span<const SelectorRoute> selectors;
    std::span<const ChannelMute> mutes;
    std::span<const ChannelVolume> volumes;
};

} // namespace ASFW::Audio::BeBoB
```

Base class executes **selectors first, then features** (two-pass) to respect FB ordering dependencies.

`std::span` requires static backing arrays:
```cpp
inline constexpr std::array kPhase88Selectors{
    SelectorRoute{0x06, 0x01},
    SelectorRoute{0x07, 0x01},
};
inline constexpr std::array kPhase88Mutes{ ... };
inline constexpr std::array kPhase88Volumes{ ... };

inline constexpr MixerMap kPhase88MixerMap{
    .selectors = kPhase88Selectors,
    .mutes = kPhase88Mutes,
    .volumes = kPhase88Volumes,
};
```

---

### `Phase88Protocol` — Phase88-Specific Overlay (~120 lines)

```cpp
class Phase88Protocol final : public BeBoBProtocol {
    const char* DeviceName() const override;
    AudioStreamRuntimeCaps DeviceCaps() const override;  // 10 PCM + 1 MIDI
    std::vector<uint32_t> SupportedRates() const override;  // {48000}
    
    void ConfigureMixer(MixerFailurePolicy, MixerCompletion) override;
    // uses kPhase88MixerMap with kBestEffort policy
    
    static constexpr MixerMap kMixerMap = kPhase88MixerMap;  // from Phase88MixerData.hpp
};
```

**Phase88 mixer init is ASFW-specific** (not in Linux/FFADO). Linux `bebob_terratec.c` only reads clock source selectors (FB 8/9); never programs mixer. FFADO `terratec_device.cpp` same — only clock source selection. The unmute/max-volume sequence is an ASFW workaround because Phase88 ships with mixer muted.

---

### `BeBoBPlug0StreamDiscovery` — Moved from `BridgeCoReadOnlyProbe`

- **From:** `Protocols/AVC/BridgeCo/BridgeCoReadOnlyProbe.hpp/.cpp`
- **To:** `Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp/.cpp`
- **Rename:** `StartPhase88ReadOnlyProbe` → `StartBeBoBPlug0Discovery`
- Entry point: takes `IAVCCommandSubmitter&`, `uint64_t guid`, completion with `DeviceModel`
- **Scope:** explicitly plug-0 + one-CMP-connection (honest name). Does NOT iterate all plugs.
- Probe FSM is unchanged for plug-0 behavior

Future generalization (separate effort): iterate advertised plug counts, build `std::vector<IsochronousPlugModel>` for inputs/outputs.

---

### `BeBoBDeviceProfiles.hpp` — Identity + Support Levels

Single source of truth (lives inside existing profile layer, NOT a new top-level table):

```cpp
enum class BeBoBSupportLevel {
    kVerified,       // full protocol, HW-tested
    kDiscoveryOnly,  // generic protocol, streaming works, no mixer/clock customization
    kExperimental,   // enabled only with explicit flag
};

struct BeBoBDevice {
    uint32_t vendorId;
    uint32_t modelId;
    const char* name;
    BeBoBSupportLevel support;
};

inline constexpr std::array kBeBoBDevices = {
    // Phase88 — verified
    BeBoBDevice{0x000aac, 0x000003, "PHASE 88 Rack FW", BeBoBSupportLevel::kVerified},
    // Known but untested — discovery only
    BeBoBDevice{0x00402b, 0x00010048, "FA-101", BeBoBSupportLevel::kDiscoveryOnly},
    BeBoBDevice{0x00402b, 0x00010049, "FA-66", BeBoBSupportLevel::kDiscoveryOnly},
    // ... etc
};

constexpr bool IsBeBoBDevice(uint32_t vendorId, uint32_t modelId) {
    return std::ranges::any_of(kBeBoBDevices, [=](const auto& d) {
        return d.vendorId == vendorId && d.modelId == modelId;
    });
}

constexpr BeBoBSupportLevel SupportLevel(uint32_t vendorId, uint32_t modelId) {
    for (const auto& d : kBeBoBDevices) {
        if (d.vendorId == vendorId && d.modelId == modelId) return d.support;
    }
    return BeBoBSupportLevel::kDiscoveryOnly;  // unknown BeBoB = discovery only
}
```

**No vendor-ID-range matching.** Single OUI can include both BeBoB-era and DICE devices. Exact match only.

---

### Per-GUID `BeBoBProfile` — Dynamic Geometry

Problem: current ADK profile registry returns raw pointers to static singletons. Mutable static `BeBoBProfile` cannot represent two devices with different geometry.

Solution: store discovered profile in the audio-device runtime, derived from immutable `AudioStreamRuntimeCaps`:

```
BeBoBPlug0StreamDiscovery
        ↓
DeviceModel / AudioStreamRuntimeCaps (per-GUID)
        ↓
GenericBeBoBProtocol (reads caps)
        ↓
per-GUID BeBoBProfile (owned by runtime)
```

Profile creation happens after discovery completes, during device publication — not from a static registry lookup.

---

### `InitialClockAnchorTimeoutMs()` — Stay in Profile

Remains in `IAudioStreamProfile`. Phase88 overrides with 4000ms. Do NOT duplicate in `BeBoBProtocol`.

---

### Namespace Leak Fix — `DiceRestartReason`

**Defined at:** `Audio/Protocols/DICE/Core/DICERestartSession.hpp:19`
**Neutral alias already exists:** `Audio/Protocols/Duplex/DuplexControlTypes.hpp:15`
  `using DuplexRestartReason = DICE::DiceRestartReason;`

**Leak sites fixed:**
| File | Fix |
|------|-----|
| `AVCAudioBackend.cpp:126` | `DuplexRestartReason::kBusResetRebind` |
| `ASFWAudioNub.cpp:677` | `DuplexRestartReason::kSampleRateChange` |

DICE-internal code (`AudioCoordinator.cpp`, `DiceAudioBackend.cpp`, `DiceRecoveryPolicy.hpp`) keeps using `DICE::DiceRestartReason` — correct, those are DICE paths.

Note: `DuplexControlTypes.hpp` aliases the entire restart state machine back to the DICE header. Full cleanup (moving all types neutral) is a separate future effort. The minimal fix for BeBoB is updating the two AV/C-side leak sites.

---

## Files to Modify

| File | Change |
|------|--------|
| `DuplexStreamProfile.hpp` | `IsTerraTecPhase88()` → `IsBeBoBDevice()` for any-iso-channel + RX-before-TX + no-pre-stream-clock-lock |
| `DeviceStreamModeQuirks.cpp` | Phase88 identity → `IsBeBoBDevice()` → `kBlocking` |
| `AVCDiscovery.cpp` | `IsTerraTecPhase88RackFw()` → `IsBeBoBDevice()`; `StartPhase88ReadOnlyProbe` → `StartBeBoBPlug0Discovery`; `PublishPhase88AudioConfig` → `PublishBeBoBAudioConfig` |
| `AudioProfileRegistry.cpp` | Register per-GUID `BeBoBProfile` (not static singleton) |
| `DeviceProtocolFactory` | `IsBeBoBDevice()` → `Phase88Protocol` for verified, `GenericBeBoBProtocol` for discovery-only; also needs timer + work queue injection |
| `TerraTecAudioProfiles.hpp` | Reference `IsBeBoBDevice()` + `kBeBoBDevices` |
| `ASFWMCPBeBoBTools.swift` | Update entry-point names |

---

## Files to Delete

| File | Reason |
|------|--------|
| `Protocols/AVC/BridgeCo/BridgeCoReadOnlyProbe.hpp` | Moved to `Audio/Protocols/BeBoB/` |
| `Protocols/AVC/BridgeCo/BridgeCoReadOnlyProbe.cpp` | Moved to `Audio/Protocols/BeBoB/` |

---

## Implementation Order

| Step | What | Dependencies |
|------|------|-------------|
| 0 | Fix `DiceRestartReason` namespace leak (AV/C-side only) | BeBoB recovery path depends on it |
| 1 | `BeBoBDeviceProfiles.hpp` — identity + support levels in profile layer | Foundation |
| 2 | `BeBoBMixerMap.hpp` — declarative mixer types with static backing arrays | Foundation |
| 3 | `Phase88MixerData.hpp` — Phase88 FB constants (static arrays) | 2 |
| 4 | Inject `ITimerScheduler` into `DeviceProtocolFactory` + `AudioRuntimeRegistry` | Infrastructure |
| 5 | Abstract `BeBoBProtocol` — extract general logic, async settle via timer, epoch/cancellation, FB framework | 1, 2, 4 |
| 6 | `Phase88Protocol` — shrink to overrides, async mixer with failure policy | 5 |
| 7 | `BeBoBPlug0StreamDiscovery` — move + rename (plug-0 scope, honest name) | 5 |
| 8 | `GenericBeBoBProtocol` — concrete fallback, discovery-derived caps | 5, 7 |
| 9 | Per-GUID `BeBoBProfile` — runtime-owned, derived from caps | 8 |
| 10 | `DeviceProtocolFactory` — dispatch verified vs discovery-only, inject timer + queue | 1, 5, 8, 9 |
| 11 | `AVCDiscovery.cpp` — `IsBeBoBDevice()` + `StartBeBoBPlug0Discovery` | 1, 7 |
| 12 | `DuplexStreamProfile.hpp`, `DeviceStreamModeQuirks.cpp` — `IsBeBoBDevice()` | 1 |
| 13 | `AudioProfileRegistry.cpp` — per-GUID profile ownership | 9 |
| 14 | Swift MCP tools — update names | 7 |
| 15 | Tests — base class mock, timer injection, epoch cancellation, ordering preserved, `IsBeBoBDevice()` | All above |

**Clean intermediate point after step 6:** architecture improved, observable Phase88 behavior unchanged.

---

## Design Decisions Summary

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Base class | Abstract `BeBoBProtocol` + concrete `GenericBeBoBProtocol` | Factory can't instantiate abstract class; support boundary unmistakable |
| Mixer config | Async `ConfigureMixer(MixerCompletion)` with `MixerFailurePolicy` | FCP operations are async; current behavior = best-effort |
| Settle timer | `Scheduling::ITimerScheduler` | `DispatchAsyncAfter` internally calls `IOSleep()` — not a real solution |
| Identity location | `DeviceProfiles/Audio/Vendors/BeBoBDeviceProfiles.hpp` | Single source of truth in existing profile layer |
| Vendor matching | Exact vendor/model only, no ranges | Single OUI can include both BeBoB-era and DICE devices |
| Support levels | `kVerified` / `kDiscoveryOnly` / `kExperimental` | Unmistakable support boundary |
| Discovery name | `BeBoBPlug0StreamDiscovery` (not `BeBoBStreamDiscovery`) | Honest about plug-0 scope; generalize later |
| Profile ownership | Per-GUID, runtime-owned, derived from `AudioStreamRuntimeCaps` | Mutable static can't represent multiple devices |
| `InitialClockAnchorTimeoutMs()` | Stays in `IAudioStreamProfile` | Don't duplicate; Phase88 overrides there |
| Mixer data location | `Phase88MixerData.hpp` only | Single source of truth |
| Cancellation | `ClockApplyEpoch` with generation + CAS guard | Safe teardown/bus-reset during async chains |
| No 1:1 copying | Fresh C++23, references for behavior only | Apache 2.0 preservation |

---

## License Compliance Checklist

- [ ] All new `.hpp`/`.cpp` files: Apache 2.0 header + `Copyright (c) 2026 ASFireWire Project`
- [ ] No verbatim code from `references/linux-sound-firewire-stack/firewire/bebob/` or `references/libffado-2.5.0/src/bebob/`
- [ ] Behavioral citations only: `// cross-validated with Linux bebob_stream.c:` + line numbers where wire-observable
- [ ] Linux device ID table regenerated from observation (factual data, not copyrightable)

---

## Testing Strategy

- Existing tests unchanged in behavior — `Phase88PreservesLinuxBeBoBCmpBeforeHostStartOrdering`, `MatchesOnlyExactPhase88Identity` still pass
- New tests for `BeBoBProtocol` base: mock FCP transport + CMP client + mock timer, verify generic start/stop lifecycle
- New test for epoch cancellation: bus reset during mixer config → exactly-once completion
- New test for `IsBeBoBDevice()`: verify it matches all ~20 devices, rejects non-BeBoB
- New test for factory dispatch: Phase88 → `Phase88Protocol`, others → `GenericBeBoBProtocol`
- New test for `ConfigureMixer` failure policy: kRequired aborts, kBestEffort continues

---

## Future Flags (Out of Scope)

- Full `DuplexControlTypes.hpp` cleanup: move all restart types neutral (not just reason enum)
- Generalize discovery: iterate all plugs, build `std::vector<IsochronousPlugModel>`
- Other `IOSleep` calls in protocol layer → `ITimerScheduler`
- Per-device clock source customization (FB 8/9 selector reads)
