# Shared Stream Session and Leases (WP-6)

Status: Implementation plan, 2026-09-14. Authoritative design document for WP-6 referenced by `documentation/plans/MIDI_IMPLEMENTATION_PLAN.md:461`.

## 1. Goal & Architectural Motivation

In the initial ASFW audio engine architecture, isochronous stream transport and the packet transmission pump were owned directly by the AudioDriverKit service (`ASFWAudioDriver` / `ASFWAudioDriverZts.cpp`). When a CoreAudio client was active, `ASFWAudioDevice::StartIO` allocated and mapped the OHCI transmit queue buffers, configured `DiceTxStreamEngine`, and hopped OHCI completion interrupts to `TxPreparationReady` via an `OSAction` to pump packets.

This coupled transmit streaming inextricably to CoreAudio:
1. **Standalone MIDI is blocked**: When no CoreAudio application is running, `StartIO` never executes, `FillTransmitSlot` returns `NotFillable`, no packets are prepared or transmitted on the FireWire bus, and MIDI cannot transmit.
2. **Fabricating HAL activity is forbidden**: Simulating fake CoreAudio clients to drive the hardware pump violates CoreAudio HAL semantics and driver integrity.
3. **Audio-only regression risk**: Shifting the pump must preserve bit-accurate audio timing, zero-timestamp (ZTS) collinearity, and low-latency audio stability without regression.

WP-6 introduces **`AudioEndpointStreamSession`** under `Audio/Engine/`, owned by `AudioCoordinator` in the core driver. The session holds the single TX consumer and content pump. Audio and MIDI become independent **leases** on this shared session.

---

## 2. Component Boundaries and Contracts

```
CoreAudio / HAL
      │
  ASFWAudioDriver   ← AudioDriverKit: HAL graph, ZTS mirror, PCM publication
      │ (AudioLease: ITxPcmSource)
  ── seam ──
      │
  AudioEndpoint     ← Holds stream session, lease state machine, TX content pump,
  StreamSession       DiceTxStreamEngine, AmdtpPacketTimeline, DextTxSlotProvider.
      │ (MidiLease: MidiTransportBlock)
  ASFWMidiNub /     ← MIDIDriverKit: UMP <-> MIDI 1.0 byte rings
  ASFWMidiDevice
      │
  IsochDuplex       ← OHCI IT/IR DMA contexts, completion handling,
  HostTransport       hardware timing, payload-opaque bus transport
```

### 2.1 The Single Pump Rule
There must remain **exactly one TX consumer and one fill pump per endpoint**. Audio-active operation and MIDI-only operation never run competing pumps. 

### 2.2 Content Independence (Linux Reference Validation)
Linux ALSA `amdtp-am824.c:353-368` writes PCM **or silence**, then MIDI unconditionally into each outgoing packet. When no PCM source is bound (MIDI-only) or when PCM is starved, `AudioEndpointStreamSession` writes valid silence for PCM slots and composes MPX-MIDI bytes into the MIDI multiplex slot. The device receives a continuous, valid AM824/CIP stream.

### 2.3 Cross-Service Lifecycles and FW-60 Prevention
- `AudioEndpointStreamSession` lives in `Audio/Engine/` in the core driver.
- It directly accesses the isochronous transmit memory descriptors allocated by `IsochService` (`txPayloadSlab_`, `txMetadataRing_`, `txControlBlock_`).
- `ASFWAudioDriver` no longer maps these raw OHCI queues. It only maps the CoreAudio direct memory buffers (`directOutputMemory_`, `directInputMemory_`, `directControlMemory_`).
- When `AudioLease` is released or `ASFWAudioDriver` stops, the session drops its view onto the audio-owned PCM source (`pcmSource_ = nullptr`) before audio buffers can be unmapped.
- Hardware transport stops only when all leases (`AudioLease` and `MidiLease`) are released.

---

## 3. Lease State Machine

```
              ┌──────────────┐
              │     IDLE     │
              └──────┬───────┘
                     │
      ┌──────────────┴──────────────┐
      │ AcquireAudioLease           │ AcquireMidiLease
      ▼                             ▼
┌──────────────┐             ┌──────────────┐
│  AUDIO ONLY  │             │  MIDI ONLY   │
│ (HW running, │             │ (HW running, │
│  PCM pumped) │             │silence+MIDI) │
└──────┬───────┘             └──────┬───────┘
       │                            │
       │ AcquireMidiLease           │ AcquireAudioLease
       └──────────────┬─────────────┘
                      ▼
              ┌──────────────┐
              │ DUPLEX BOTH  │
              │ (HW running, │
              │   PCM+MIDI)  │
              └──────┬───────┘
                     │
      ┌──────────────┴──────────────┐
      │ ReleaseAudioLease           │ ReleaseMidiLease
      ▼                             ▼
┌──────────────┐             ┌──────────────┐
│  MIDI ONLY   │             │  AUDIO ONLY  │
└──────┬───────┘             └──────┬───────┘
       │                            │
       │ ReleaseMidiLease           │ ReleaseAudioLease
       └──────────────┬─────────────┘
                      ▼
              ┌──────────────┐
              │     IDLE     │
              │ (HW stopped) │
              └──────────────┘
```

1. **Idle -> Streaming**:
   When the first lease is acquired (`AudioLease` or `MidiLease`), `AudioDuplexCoordinator::StartStreaming(endpointId)` starts the hardware transport. If start fails, the lease acquisition rolls back and returns an error.
2. **Dynamic Binding**:
   - Acquiring `AudioLease` while `MidiLease` is active binds `ITxPcmSource` to the running engine without halting or restarting hardware.
   - Releasing `AudioLease` while `MidiLease` is active unbinds `ITxPcmSource` (`pcmSource_ = nullptr`), immediately continuing with valid silence and MIDI.
3. **Streaming -> Idle**:
   When the last lease is released, `AudioDuplexCoordinator::StopStreaming(endpointId)` stops the hardware transport.

---

## 4. Deliverables and Detailed Work Breakdown

1. **`ASFWDriver/Audio/Engine/AudioEndpointStreamSession.hpp` & `.cpp`**:
   - Manages `EndpointId`, `AudioEndpointRuntime&`, `AudioDuplexCoordinator&`, `IsochService&`, `IIsochDuplexHostTransport&`.
   - Owns `DiceTxStreamEngine` (primary and optional secondary stream).
   - Owns `DextTxSlotProvider` mapped directly from `IsochService` descriptors.
   - Encapsulates `ObserveTxHardware`, `PrepareTransmitSlots`, and the content fill loop.
   - Encapsulates `AcquireAudioLease`, `ReleaseAudioLease`, `AcquireMidiLease`, `ReleaseMidiLease`.
2. **`ASFWDriver/Audio/Core/AudioCoordinator.hpp` & `.cpp`**:
   - Instantiates `AudioEndpointStreamSession` during endpoint setup.
   - Routes `StartStreaming` / `StopStreaming` to `AudioLease`.
   - Exposes `StartMidiStreaming` / `StopMidiStreaming` for `MidiLease`.
   - Connects `ASFWMidiNub` transport block directly to the session.
3. **`ASFWDriver/Audio/DriverKit/ASFWAudioNub` & `ASFWAudioDriver`**:
   - Remove raw TX queue allocation and mapping from `ASFWAudioDevice::StartIO`.
   - Remove obsolete MIDI relay methods from `ASFWAudioNub`.
   - Retain CoreAudio HAL ZTS publication (`PublishSharedZeroTimestampToHAL`) in `ASFWAudioDriverZts.cpp`.
4. **`ASFWDriver/Midi/DriverKit/ASFWMidiNub` & `ASFWMidiDevice`**:
   - Acquire `MidiLease` when MIDI transport is armed / active.
   - Release `MidiLease` when MIDI transport is quiesced.
5. **Unit Tests**:
   - `tests/audio/AudioEndpointStreamSessionTests.cpp` testing:
     - Standalone MIDI lease start & silence generation.
     - Seamless dynamic audio lease addition / removal.
     - Last lease hardware stop.
     - Failed start rollback.
