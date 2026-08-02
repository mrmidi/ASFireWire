# Design Note: Unified Device-Backend Interface

**Status:** design note / proposal (2026-07-11). Not an accepted decision like
[`SAMPLE_RATE_EXPANSION.md`](SAMPLE_RATE_EXPANSION.md); it records a direction
and the decisions it forces. No code implied by this note.

## The idea in one sentence

A caller should be able to ask a device a **protocol-neutral question** — *"what
is your current sample rate?"*, *"set 44.1 kHz"*, *"how many input channels?"* —
and the device answers. The caller must **not** know whether the answer came from
a DICE register read, an AV/C plug/stream-format query, an RME vendor register, or
a future Oxford/BeBoB path. Protocol is an implementation detail behind one
interface, not a branch in the caller.

This is the opposite of the 2000s model where each vendor shipped a full custom
driver and chose its own way end-to-end. Here there is **one shared audio engine**
(cadence → AMDTP/CIP → OHCI) and **thin protocol adapters** that only translate
neutral requests into device-specific transactions.

### Motivating example: "what's the current sample rate?"

One neutral call, three unrelated implementations, caller unaware:

```
GetCurrentSampleRate() -> Result<uint32_t>
```

- **DICE** reads the GLOBAL section clock/status registers (`GlobalOffset::kSampleRate`
  `0x5C` / nominal-rate bits in `kStatus` `0x54`, `DICETypes.hpp`).
- **AV/C** issues a plug-signal-format / stream-format query on the Music subunit.
- **RME** reads its vendor status register (the legacy kext's `hwGetRate` — see
  [`44100.md`](44100.md) §12.1).

The caller gets a `uint32_t` Hz and never sees any of that. `SetSampleRate(hz)`,
`GetSupportedRates()`, `GetChannelCounts()`, `GetClockSource()` follow the same
shape.

## This is a convergence, not a greenfield

The universal-engine + protocol-adapter architecture **already exists in ASFW** —
it is partly built and needs unifying, not inventing. Today's seams:

| Concern | Existing seam | State |
|---|---|---|
| Control plane (start/stop streaming) | `IAudioBackend` (`Name`, `StartStreaming(guid)`, `StopStreaming(guid)`) | DICE + AV/C backends implement it |
| Per-device business logic (bring-up, DSP, controls, caps) | `IDeviceProtocol` | DICE, Focusrite-DICE, Oxford/Apogee behind it |
| Capability / timing data (formats, channels, DBS, per-rate latency) | `IAudioDeviceProfile` | protocol-agnostic; already rate-parameterized (`double sampleRate`) |
| Identity → which protocol | `DeviceProtocolFactory` + `AudioIntegrationMode` + `DeviceProfiles/Audio/` | registry of Focusrite/Apogee/Alesis/Midas/PreSonus |

So the vision is mostly realized structurally. The work is to make the interface
**honest and neutral**, then add RME as one more adapter.

## Why it is not unified yet

The generic interfaces have accreted DICE-specific shape — the interface leaks the
very protocol it is supposed to hide:

- `IDeviceProtocol` hardcodes DICE and 48 kHz in its bring-up hooks:
  `PrepareDuplex48k`, `ProgramRxForDuplex48k`, `ProgramTxAndEnableDuplex48k`,
  `ConfirmDuplex48kStart`, plus an `AsDiceDuplexProtocol()` downcast. A neutral
  interface cannot contain `...48k` or `AsDice...`.
- The "universal" `IAudioBackend` is thin (start/stop by GUID); the real surface —
  `RequestClockConfig`, `HandleRecoveryEvent`, `BeginTeardown` — lives as
  DICE-specific public methods on the concrete `DiceAudioBackend` and is reached by
  downcasting. So the neutral contract today is essentially just start/stop, and
  everything interesting is protocol-specific.

**ASFW's real risk is not per-vendor forks (that is already avoided) — it is the
generic interface slowly becoming "DICE with a vtable" so AV/C and RME never
genuinely fit.** The `...48k` hooks are that drift already in progress.

## The unified surface (neutral query/command model)

Collapse the leaks into one protocol-neutral device interface whose operations are
all phrased as questions and commands, never as protocol mechanics. Illustrative
shape only:

```
Probe()                 -> Result<DeviceCapabilities>   // rates, channels, clock sources, quirks
GetCurrentSampleRate()  -> Result<uint32_t>
SetSampleRate(hz)       -> Status                        // async under the hood; see boundaries
GetClockSource()        -> Result<ClockSource>
SetClockSource(src)     -> Status
ConfigureStreams(req)   -> Result<ActiveStreamProfile>   // the neutral profile the engine consumes
StartStreaming() / StopStreaming() -> Status
```

The device answers `ActiveStreamProfile` — a **value type** (no pointers into
either service's memory; FW-60 rule) that the shared engine consumes to pick the
cadence source and drive AMDTP/OHCI. The engine downstream of the profile is 100%
protocol-agnostic.

## Design rules the unification must hold

1. **Two-protocol rule.** A method may live on the neutral interface only if at
   least two *unrelated* protocols implement it meaningfully. Force DICE **and**
   AV/C through every neutral method; anything only DICE can answer belongs on the
   concrete adapter, not the shared interface.
2. **No protocol names in the neutral surface.** `...48k`, `AsDice...`, register
   offsets, plug numbers, and vendor opcodes are leaks. Neutral requests are
   rate-generic and mechanism-free (`PrepareDuplex(profile)`, not
   `PrepareDuplex48k`). De-leaking `IDeviceProtocol` is the same task as
   rate-genericizing DICE — [`SAMPLE_RATE_EXPANSION.md`](SAMPLE_RATE_EXPANSION.md)
   gap #5.
3. **Async is part of the contract, not hidden.** DICE clock/rate changes are async
   FireWire transactions that can take seconds to lock (`DiceDuplexRestartCoordinator`).
   Neutral setters must be modeled as async (completion/continuation), or a
   synchronous signature re-imports the multi-second work-queue stall flagged in
   the PR #41 review. "Answer the question" may mean "answer later."
4. **The profile owns per-rate device timing.** `IAudioDeviceProfile` already
   carries per-rate transfer delay (`TxTransferDelayTicks(sampleRate)`, default flat
   `12800`). The device-specific presentation lead — the capture-gated value from
   the cadence work ([`44100.md`](44100.md) §12.4, `SAMPLE_RATE_EXPANSION.md` §8) —
   lives here, per protocol, per rate. It is a profile override, not a new
   interface.

## Layer boundary (what the backend must not do)

The backend **configures the physical device and answers questions about it. It
does not touch the stream.** Keep out of every adapter:

- AMDTP/CIP framing, DBC state, SYT arithmetic
- RX replay and the rational cadence engine
- OHCI descriptors / DMA layout / isoch scheduling
- CoreAudio timing anchors (ZTS / `HostClockAnchor`)
- bus-reset recovery state machine

Keep inside the adapter: DICE register transactions, AV/C + CMP/PCR, RME vendor
protocol, clock-source selection, capability discovery, and the firmware quirks
needed only to start/stop. This is the existing layer-boundary rule (CLAUDE.md),
restated for backends.

## Where new protocols slot in

RME (and any future class) is one more adapter, not a new architecture: a
`AudioIntegrationMode` value, an `RmeAudioBackend : IAudioBackend`, an
`RmeProtocol : IDeviceProtocol`, and profiles under `DeviceProfiles/Audio/Vendors/`.
This is only clean **after** the de-leak (rules 1–2); adding RME against today's
DICE-shaped interface would fight the `48k`/`AsDice` accretion.

## DICE-first, and a simulated DICE backend

- **Generalize from DICE.** It is the most complete adapter and TCAT spans many
  vendors, so shape the neutral surface around what DICE needs, then validate it by
  forcing AV/C through the same methods (rule 1).
- **Add a simulated DICE backend behind the neutral interface.** It implements the
  two interfaces and feeds canned RX sequences, letting the whole
  `profile → cadence → AMDTP → OHCI` chain (including 44.1 replay) be host-tested
  with no hardware — the transport-side analogue of `ADKVirtualAudioLab` and the
  `Testing/` DMA stubs. Cheap because the seams already exist, and it makes the
  de-leak refactor verifiable off-hardware. Likely the highest-leverage first step.

## Open decisions

1. Does the neutral contract live on one interface, or stay split as control-plane
   (`IAudioBackend`) + device-logic (`IDeviceProtocol`)? Today the split is
   ambiguous and the real surface leaks through concrete backends.
2. Exact neutral vocabulary for clock source, multi-stream devices (a profile is a
   *set* of streams per direction, e.g. Venice F32 = 2×16 — the single
   `rxChannel`/`txChannel` shape is insufficient), and async completion.
3. Whether `AudioIntegrationMode` stays an enum or becomes a capability set as the
   protocol count grows.
