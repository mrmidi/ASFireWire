# AUDIO_BACKENDS_CONTROLS — mapping CoreAudio/ADK controls onto device backends

**Status:** design note. Scope: **Apogee Duet FireWire first** (Oxford OXFW backend). The
architecture is meant to generalise to BeBoB/DICE, but every concrete mapping below is Duet.

**Thesis: backend = control.** An ADK control object is a *presentation* of a backend parameter,
not a place to put logic. `ApogeeDuetProtocol` already owns the whole Duet control plane —
vendor-command encode/decode, parameter structs, ranges, meters. What is missing is a generic
bridge from ADK control objects onto that existing async parameter API. We should build **one**
bridge driven by per-device descriptor tables, not a per-device ADK subclass.

---

## 1. The ADK control surface (authoritative — DriverKit 25.5 SDK headers)

Hierarchy (all control classes are `LOCALONLY`, so they are subclassable in-process and their
`HandleChange*` virtuals are ordinary C++ overrides):

```
OSObject
└── IOUserAudioObject                       AddCustomProperty()
    ├── IOUserAudioClockDevice              AddControl() / RemoveControl()
    │   └── IOUserAudioDevice
    ├── IOUserAudioControl
    │   ├── IOUserAudioLevelControl         float dB or scalar
    │   ├── IOUserAudioBooleanControl       bool
    │   ├── IOUserAudioSelectorControl      uint32 + value descriptions
    │   ├── IOUserAudioSliderControl        uint32 + settable range
    │   └── IOUserAudioStereoPanControl     float
    ├── IOUserAudioCustomProperty           arbitrary typed property
    ├── IOUserAudioStream
    └── IOUserAudioBox
```

`AddControl`/`RemoveControl` live on `IOUserAudioClockDevice` (`IOUserAudioClockDevice.iig:806`,
`:823`) and are inherited by `IOUserAudioDevice` — which is why `audioDevice.AddControl(...)` at
`Controls/AudioControlBuilder.cpp:119` resolves. **`RemoveControl` existing means the control set
can be changed after publication** — see §6 for why that matters.

### Class IDs (`AudioDriverKitTypes.h:558-588`)

| Class ID | Backing class | What CoreAudio does with it |
|---|---|---|
| `VolumeControl` `'vlme'` | LevelControl | Sound Settings slider, volume keys (master, element 0) |
| `LFEVolumeControl` `'subv'` | LevelControl | LFE trim |
| `LevelControl` `'levl'` | LevelControl | generic level, no special UI |
| `MuteControl` `'mute'` | BooleanControl | mute key, Sound Settings mute |
| `SoloControl` `'solo'` | BooleanControl | per-element solo |
| `LFEMuteControl` `'subm'` | BooleanControl | — |
| `JackControl` `'jack'` | BooleanControl | jack-sense (read-only presence) |
| `PhantomPowerControl` `'phan'` | BooleanControl | Audio MIDI Setup |
| `PhaseInvertControl` `'phsi'` | BooleanControl | Audio MIDI Setup |
| `ClipLightControl` `'clip'` | BooleanControl | clip indicator |
| `TalkbackControl` `'talb'` / `ListenbackControl` `'lsnb'` | BooleanControl | — |
| `DataSourceControl` `'dsrc'` | SelectorControl | input/output source picker (AMS) |
| `DataDestinationControl` `'dest'` | SelectorControl | destination picker |
| `ClockSourceControl` `'clck'` | SelectorControl | clock source (AMS) |
| `LineLevelControl` `'nlvl'` | SelectorControl | nominal line level |
| `HighPassFilterControl` `'hipf'` | SelectorControl | HPF |
| `SliderControl` `'sldr'` | SliderControl | generic, re-rangeable |
| `StereoPanControl` `'span'` | StereoPanControl | pan |

Scopes: `Global 'glob'`, `Input 'inpt'`, `Output 'outp'`, `PlayThrough 'ptru'`
(`AudioDriverKitTypes.h:185-191`). Element `0` = `IOUserAudioObjectPropertyElementMain`
(`:216`), `1..N` = per-channel.

### The three verbs

1. **host → device**: override `HandleChangeDecibelValue(float)` / `HandleChangeScalarValue(float)`
   / `HandleChangeControlValue(bool|uint32|float)` / `HandleChangeSelectedValues(...)`. Default
   implementations just store the value; ours must route to the backend **and** then store.
2. **device → host**: call `SetDecibelValue()` / `SetControlValue()` / `SetCurrentSelectedValues()`.
   Per the headers these "send a notification to the host to update the object state", so this is
   the push path for hardware-initiated changes. It does **not** re-enter `HandleChange*`.
3. **lifecycle**: `AddControl` / `RemoveControl`.

### Two constraints that shape the design

- **`IOUserAudioLevelControl` has no `SetRange`.** Its dB range is fixed at `Create()`
  (`IOUserAudioLevelControl.iig:104-110`). `IOUserAudioSliderControl` *does* have `SetRange`
  (`IOUserAudioSliderControl.iig:264`). So a control whose **range depends on a mode** cannot be a
  re-ranged LevelControl — it must either declare a superset range, be swapped via
  `RemoveControl`+`AddControl`, or be a SliderControl. This bites the Duet input gain (§4).
- **Scope must be part of the routing key.** Input and output volume are both `'vlme'` on element
  `0`, differing only by scope. The existing boolean routing key is `(classIdFourCC, element)` only
  (`Controls/ASFWProtocolBooleanControl.iig:45-49`, `AudioControlBuilder.cpp:84-85`) — that
  collides the moment we have controls of the same class on both scopes.

---

## 2. Duet backend capability inventory

Reference: `references/alsa-userspace-control-protocols-impl/protocols/oxfw/src/apogee.rs`
(signal flow `:8-22`). Our port: `ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeTypes.hpp`,
`ApogeeDuetProtocol.{hpp,cpp}`.

Signal flow — 2 analog in, 4×2 mixer, 2 analog out:

```
xlr-1 ──or──> analog-input-1 ─┬──────────────> stream-output-1/2
phone-1 ──┘                   │  │
xlr-2 ──or──> analog-input-2 ─┼──┤
phone-2 ──┘                   v  v
                          ++=========++
stream-input-1/2 ───────> ||  4 x 2  || ──or──> analog-output-1/2
                          ||  mixer  ||   ^
                          ++=========++   └── output source selector
```

The backend already exposes every group as an **async** Get/Set pair
(`ApogeeDuetProtocol.hpp:147-183`):

| Group | Backend accessor | Wire mechanism | Contents & ranges |
|---|---|---|---|
| Knob | `GetKnobState` / `SetKnobState` | AV/C vendor `HwState 0x07`, 11 bytes | output mute, knob target, output volume `0..64`, input gains `[2] 10..75` |
| Output | `GetOutputParams` / `SetOutputParams` | vendor `OutMute 0x09`, `OutVolume 0x15`, `OutSourceIsMixer 0x11`, `OutIsConsumerLevel 0x04`, `Mute/UnmuteFor{Line,Hp}Out 0x16-0x19` | mute, volume `0..64`, source, nominal level, line/HP mute modes |
| Input | `GetInputParams` / `SetInputParams` | vendor `InGain 0x05`, `MicPolarity 0x00`, `XlrIsMicLevel 0x01`, `XlrIsConsumerLevel 0x02`, `MicPhantom 0x03`, `InputSourceIsPhone 0x0C`, `InClickless 0x1E` | gains `[2] 10..75`, polarities `[2]`, XLR nominal levels `[2]`, phantom `[2]`, sources `[2]`, clickless |
| Mixer | `GetMixerParams` / `SetMixerParams` | vendor `MixerSrc 0x10` | 2 dst × (2 analog + 2 stream) coefficients, `0..0x3fff` |
| Display | `GetDisplayParams` / `SetDisplayParams`, `ClearDisplay` | vendor `0x13`, `0x14`, `0x1B`, `0x22` | target, mode, overhold |
| Meters | `GetInputMeter` / `GetMixerMeter` | **async block read**, not AV/C: `0xFFFFF0080000 + 0x0004` (8 B), `+ 0x0404` (16 B) | input `[2]`, stream-in `[2]`, mixer-out `[2]`, `int32` |

Our port is faithful on the one non-obvious encoding: the knob's output-volume byte is **inverted
on the wire** (`raw[3] = 64 − volume`), handled at `ApogeeDuetProtocol.cpp:1258` and matching
`apogee.rs:224` / `:250`.

**dB semantics from the reference** (`apogee.rs:298-311`, `:465-494`):
- Output volume `0..64` ⇒ **−64..0 dB**, 1 dB per step. Clean linear dB.
- Output nominal level `Consumer` = −10 dBV, `Instrument` = fixed level for an external amp.
  ⚠️ Our comment at `ApogeeTypes.hpp:46` says `Instrument = +4 dBu` — that is not what the
  reference says (`+4 dBu` is the *input* `Professional` level). Comment bug, worth fixing.
- Input gain `10..75` ⇒ 10..75 dB when XLR source + `Microphone` nominal level. For the
  `Phone` source the reference documents 0..65 dB — i.e. **the dB meaning of the same raw range
  shifts with the selected source**.
- Input `Professional` (+4 dBu) and `Consumer` (−10 dBV) are **fixed-gain**: the gain value is
  inert in those modes.

---

## 3. Duet → ADK mapping (the proposal)

Element `0` = main/master, `1`/`2` = channels. All settable unless noted.

| # | ADK control | Class ID | Scope | Elem | Backend target | Mapping |
|---|---|---|---|---|---|---|
| 1 | LevelControl | `'vlme'` | Output | 0 | `OutputParams.volume` | dB range `{−64, 0}`; `raw = 64 + round(dB)`, clamp `0..64` |
| 2 | BooleanControl | `'mute'` | Output | 0 | `OutputParams.mute` | direct |
| 3 | SelectorControl | `'dsrc'` | Output | 0 | `OutputParams.source` | `stream-input-1/2` / `mixer-output-1/2` |
| 4 | SelectorControl | `'nlvl'` | Output | 0 | `OutputParams.nominalLevel` | `instrument` / `−10 dBV` |
| 5 | LevelControl | `'vlme'` | Input | 1,2 | `InputParams.gains[i]` | see §4 — range depends on source |
| 6 | BooleanControl | `'phan'` | Input | 1,2 | `InputParams.phantomPowerings[i]` | already mapped (`TryMapBooleanControl`) |
| 7 | BooleanControl | `'phsi'` | Input | 1,2 | `InputParams.polarities[i]` | already mapped |
| 8 | SelectorControl | `'dsrc'` | Input | 1,2 | `InputParams.sources[i]` | `xlr` / `phone` |
| 9 | SelectorControl | `'nlvl'` | Input | 1,2 | `InputParams.xlrNominalLevels[i]` | `mic` / `+4 dBu` / `−10 dBV` |

Items 1, 2, 6, 7 are the ones that produce visible macOS behaviour (Sound Settings slider, volume
and mute keys, Audio MIDI Setup toggles). Items 3, 4, 8, 9 surface in Audio MIDI Setup.

### Does not map to any ADK control

| Duet feature | Why | Option |
|---|---|---|
| Mixer 4×2 coefficients | ADK has no matrix-mixer control | `IOUserAudioCustomProperty`, or leave to a future ASFW app UI over the user client |
| Input / mixer meters | ADK has no meter control | CustomProperty; or drive `'clip'` BooleanControl from a threshold |
| Display target/mode/overhold | device-specific UI | CustomProperty or app-only |
| Line/HP mute modes (knob-push behaviour) | no CoreAudio concept | CustomProperty or app-only |
| Knob target | reports which param the physical knob drives | read-only CustomProperty at most |
| Clickless | no CoreAudio concept | app-only |

The honest split: **CoreAudio gets what CoreAudio understands; everything else belongs to the ASFW
app over the user client.** Do not contort ADK controls to carry mixer state.

---

## 4. The input-gain range problem

Duet input gain is `10..75` raw, but its dB meaning depends on `sources[i]` (XLR ⇒ 10..75 dB,
Phone ⇒ 0..65 dB) and it is **inert** when `xlrNominalLevels[i]` is `Professional`/`Consumer`.
Since `IOUserAudioLevelControl` has no `SetRange`, the options are:

- **(a) Fixed superset range `{0, 75}` dB** — simplest, always present, slightly dishonest at the
  edges. Recommended for a first cut.
- **(b) Swap the control** on source change: `RemoveControl` + `Create` + `AddControl`. Honest
  ranges, but needs the ADK re-publication contract validated (§6) and risks HAL churn.
- **(c) `SliderControl`** with `SetRange` — honest and re-rangeable, but it is a raw `uint32` with
  no dB semantics, so it gets no meaningful Sound Settings UI.

Whichever is chosen, `isSettable` should reflect reality: report the gain control as non-settable
when the nominal level makes it inert, rather than silently accepting writes.

---

## 5. Architecture

### 5.0 Prerequisite: the control seam is currently severed

`ASFWAudioDevice::PopulateNubProperties()` (`Audio/Model/ASFWAudioDevice.hpp:60-133`) does not
publish `kHasPhantomOverride`, `kPhantomSupportedMask`, `kPhantomInitialMask`, or
`kBoolControlOverrides`. The consumer parses all four (`Config/AudioDriverConfig.cpp:59`,
`:136-164`) and `AVCDiscovery::ConfigureDuetPhantomOverrides` (`Protocols/AVC/AVCDiscovery.cpp:132-172`)
fills them — the values are dropped in between. Regression from `889b06a4` (2026-06-11), which
stripped the publish list; every other key it removed was restored piecemeal, these four never
were. Net effect: `boolControlCount` is always 0, so the Duet's phantom/polarity controls have not
reached CoreAudio since June. **Fix this first — every control below rides the same channel.** Add
a publish→parse round-trip test; the existing tests all sit on one side of the cut, which is why a
green suite never caught it.

### 5.1 Generalise the descriptor, don't add a parallel one

Today's `BoolControlDescriptor` (`Config/AudioDriverConfig.hpp:32-38`) is bool-specific. Adding
`LevelControlDescriptor`, `SelectorControlDescriptor`, … alongside it duplicates the slot array,
the count, the reset, and the graph wiring four times. Instead: one tagged descriptor.

```
enum class ControlKind : uint32_t { Boolean, Level, Selector };

struct ControlDescriptor {
    ControlKind kind;
    uint32_t classIdFourCC;      // 'vlme' | 'mute' | 'phan' | 'dsrc' | ...
    uint32_t scopeFourCC;        // 'inpt' | 'outp' | 'glob'   <- part of the routing key
    uint32_t element;            // 0 = main
    bool     isSettable;
    union {                      // kind-specific payload
        struct { bool initial; } boolean;
        struct { float initialDb, minDb, maxDb; } level;
        struct { uint32_t initial, count; /* value descriptions */ } selector;
    };
};
```

The routing key becomes `(kind, classId, scope, element)` — fixing the input/output `'vlme'`
collision noted in §1.

### 5.2 Protocol interface: one typed control accessor, not three-per-type

`IDeviceProtocol` currently carries `SupportsBooleanControl` / `GetBooleanControlValue` /
`SetBooleanControlValue` (`Audio/Protocols/IDeviceProtocol.hpp:129-159`). Extending that pattern
per type gives nine virtuals. Prefer a single typed value:

```
struct ControlValue { ControlKind kind; union { bool b; float f; uint32_t u; }; };

virtual bool     SupportsControl(const ControlKey&) const;
virtual IOReturn ReadControl (const ControlKey&, ControlValue& out);
virtual void     WriteControl(const ControlKey&, ControlValue, VoidCallback);   // async!
```

For the Duet these fan out onto the existing `Get*Params`/`Set*Params` calls — no new wire code.

### 5.3 Async discipline — the current pattern must not be reused

`ApogeeDuetProtocol::Get/SetBooleanControlValue` (`ApogeeDuetProtocol.cpp:1017-1098`) spin-waits
`IOSleep(1)` for up to `kControlSyncTimeoutMs = 1500` on the caller's queue. Acceptable for a
one-shot phantom toggle; unacceptable for volume — F11/F12 emit ~16 steps per key-hold and the
Sound Settings slider drags continuously, so each step would block the nub queue for a full FCP
round-trip. This is the same hazard family as the CMP stage work-loop self-deadlock.

Required instead:
- **`WriteControl` is fire-and-forget.** `HandleChange*` updates the ADK control value
  optimistically, posts the write, returns `kIOReturnSuccess` immediately.
- **Latest-value-wins coalescing** per control key: one write in flight; a change arriving while
  one is pending replaces the pending value rather than queueing.
- **Diff-based flush**, exactly as the reference does — `DuetFwProtocol::update` compares new vs
  previous params and sends only the commands whose values differ
  (`protocols/oxfw/src/apogee.rs:74-88`). Since the Duet's setters take a whole params struct, this
  is what keeps one volume tick from re-sending six vendor commands.
- **Reads are cached, not synchronous.** Seed at publish time from the shadow copy; never block a
  HAL property read on a wire round-trip.

### 5.4 Device → host: the Duet has no notification, so poll

The reference confirms there is **no knob-change notification**:
`NotifyModel::parse_notification` handles only bus-lock, while `MeasureModel::measure_states`
re-reads `HwState` every measure cycle and copies knob output-mute/volume into the output params
and knob input-gains into the input params (`runtime/oxfw/src/apogee_model.rs:108-128`,
`:130-144`). So a physical knob turn is only observable by polling.

One `HwState` read returns output mute + output volume + both input gains, so a single periodic
AV/C status command feeds controls 1, 2 and 5. On change, push with `SetDecibelValue()` /
`SetControlValue()` — which notifies the HAL without re-entering `HandleChange*`.

Mechanics: `ITimerScheduler::ScheduleAfter` (`Scheduling/ITimerScheduler.hpp:24`) for the poll,
and the existing `DeviceClockChanged(OSAction*, uint32_t)` bridge
(`Audio/DriverKit/ASFWAudioDriver.iig:50-54`) is the precedent for getting a device-side event
across to the audio driver. Per the hot-path/instrumentation rule the poll must be silent unless a
value actually changes. Poll only while the device is open, and suppress the echo of our own
just-written value to avoid a feedback loop with in-flight writes.

**Open:** poll interval. ALSA's measure cycle is ~50 ms; that is 20 FCP transactions/second
forever. Start slower (250–500 ms) and only tighten if knob tracking feels laggy — a knob is a
human-scale control and the wire cost is real.

---

## 6. Needs validation before committing to the design

Cheap, in `ADKVirtualAudioLab/` (no FireWire hardware — and note the lab has **no** control code
today, so this is greenfield there):

1. Do the volume/mute **keys** actually bind to a settable master `'vlme'` + `'mute'` on output
   scope element 0? Does a per-channel-only control grey them out? (Asserted in `VOLUME_TODO.md`,
   never verified.)
2. Is `AddControl` legal **after** the device is published / while running? Is
   `RemoveControl` + re-`AddControl` tolerated by the HAL, or does it require a configuration
   change? This decides §4 option (b).
3. Does `SetDecibelValue()` from a non-HAL context (our poll callback) propagate cleanly, and does
   it avoid re-entering `HandleChangeDecibelValue`?
4. How does Sound Settings render a LevelControl whose dB range is positive-only (`{10, 75}`)?

Needs the Duet on the wire:

5. Does the device honour `OutVolume` while nominal level is `Instrument`? The reference exposes
   volume as unconditionally settable and does not gate it (`apogee_model.rs:458-464`), so the
   physical behaviour is unknown.
6. Confirm the `0..64` ⇒ `−64..0 dB` mapping is perceptually linear-in-dB rather than a raw taper.

⚠️ One reference bug to *not* mirror: `apogee_model.rs:480-493` writes `params.source` from
`Self::SOURCES` in the `OUTPUT_NOMINAL_LEVEL_NAME` branch — a copy-paste error; it should set
`params.nominal_level`. Our mapping must set the nominal level.

---

## 7. Phasing

| Phase | Work | Notes |
|---|---|---|
| **0** | Restore the four missing keys in `PopulateNubProperties` + round-trip test | Fixes the phantom/polarity regression on its own; unblocks everything |
| **1** | Lab validation §6.1-6.4 in `ADKVirtualAudioLab` | Settles the master-element and re-publication questions before any driver code |
| **2** | Generalise `BoolControlDescriptor` → `ControlDescriptor`; scope in the routing key; migrate the existing bool path (no new behaviour) | Pure refactor, keeps one path per CLAUDE.md |
| **3** | `ASFWProtocolLevelControl`; `ControlValue` on `IDeviceProtocol`; async + coalesced `WriteControl`; retire the `IOSleep` spin-wait | Duet output volume + mute live here |
| **4** | `HwState` poll → `SetDecibelValue`/`SetControlValue` push | Physical knob reaches CoreAudio |
| **5** | `ASFWProtocolSelectorControl`; output/input source + nominal level; input gain per §4(a) | |
| **6** | Mixer/meters/display via CustomProperty or the ASFW app | Only if wanted; not CoreAudio's business |

Regenerate `ASFW.xcodeproj` (`xcodegen generate`) with each phase that adds `.iig`/`.cpp` files,
and commit the generated project alongside `project.yml`.

---

## 8. Generalisation notes (not this pass)

The same descriptor + `ControlValue` machinery should absorb the other backends, which is the test
of whether the abstraction is right:

- **BeBoB / Phase 88** — AV/C Feature Blocks give volume/mute per FB channel
  (`BeBoBProtocol.cpp:273-310`), Selector Blocks give routing (`:256`). Caveat already documented
  in `VOLUME_TODO.md`: the Phase 88's only attenuators sit on the mixer path, and only one output
  pair can be mixer-fed at a time, so a *master* volume there is not honestly a hardware control.
- **DICE / Saffire** — register writes; the Focusrite/TCAT mixer surface is a matrix, i.e. mostly
  the CustomProperty bucket rather than ADK controls.
- Reading `CtlAttr::Minimum/Maximum/Resolution` (`0x02`/`0x03`/`0x01`, per
  `references/alsa-userspace-control-protocols-impl/protocols/ta1394/audio/src/lib.rs:80-107`) to
  discover real ranges instead of hardcoding them needs
  `AudioFunctionBlockCommand::BuildCdb` to stop hardcoding the attribute to `0x10` Current
  (`Protocols/AVC/AudioFunctionBlockCommand.cpp:70`).
