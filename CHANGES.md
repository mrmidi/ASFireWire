# ASFireWire — Changes & Fix Log (fork by cube666999)

Fork: https://github.com/cube666999/ASFireWire-by-cube666999  
Base: https://github.com/mrmidi/ASFireWire  
Test device: MOTU 828 MK3 (target), developed with Claude Code  
Tests: 488/488 passing

---

## Implementation Status (May 2026)

| Subsystem | Status | Notes |
|-----------|--------|-------|
| OHCI init & bus reset | ✅ Working | Self-ID, topology, gap count |
| Async TX/RX | ✅ Working | Block read/write, lock, PHY |
| Config ROM reading | ✅ Working | Full FSM multi-node scanner |
| AV/C / FCP | ✅ Working | Music Subunit, PCR space, `SendSampleRateCommand` (0x19) |
| IRM | ✅ Working | Election, channel + bandwidth allocation |
| Isoch Transmit (IT) | ✅ Working | AM824 + SYT + cadence |
| Isoch Receive (IR) | 🚧 WIP | Pipeline exists, needs hardware validation |
| AudioDriverKit | 🚧 In progress | `ASFWAudioDriver` + `ASFWAudioNub` wired; `HandleChangeSampleRate` implemented |

---

## Fixes (27 commits)

### Fix 1 — ConnectOPCR: channel not written to oPCR register (critical)
**File:** `ASFWDriver/Protocols/AVC/CMP/CMPClient.cpp`

`ConnectOPCR(plug, callback)` called `PerformConnect(..., setChannel=nullopt, ...)` —
incremented p2p counter but never wrote the channel field to the oPCR register.

Per IEC 61883-1 §10.4.2: the controller MUST write the channel to oPCR on p2p connect
(same as `ConnectIPCR` already did correctly).

**Fix:** added `uint8_t channel` parameter to `ConnectOPCR`, passed as `setChannel`.  
**Tests:** +4 tests in `CMPClientTests`.

---

### Fix 2 — IRM: AllocateResources never called before CMP connect (critical)
**File:** `ASFWDriver/Audio/Backends/AVCAudioBackend.cpp/.hpp`

Channels were hardcoded (`kDefaultIrChannel=0`, `kDefaultItChannel=1`).
`IRMClient::AllocateResources` was never called — bandwidth was never reserved on the bus.

For MOTU 828 MK3 at 48 kHz, 18 channels, S400: ~146 bandwidth units required per IEC 61883-1.

**Fix:** added `SetIRMClient(IRMClient*)`, call `AllocateResources(ch, bw, cb)` before
`StartReceive`, release on stop. Dynamic channel passed through CMP connect sequence.  
**Tests:** +12 tests in `IRMClientTests` + `CMPClientTests`.

---

### Fix 3 — oPCR read-back after ConnectOPCR
**File:** `ASFWDriver/Audio/Backends/AVCAudioBackend.cpp`

After CAS `ConnectOPCR`, driver now reads back oPCR[0] to verify the channel was
actually written. Detects silent CAS failures.  
**Tests:** +3 tests (ReadOPCR OK / fail / invalid-plug).

---

### Fix 4 — Bus reset recovery in AudioCoordinator
**File:** `ASFWDriver/Audio/AudioCoordinator.cpp/.hpp`

Bus reset terminates all isochronous connections per IEEE 1394 §8.3.
`OnDeviceSuspended` stops the backend and records the GUID.
`OnDeviceResumed` calls `StartStreaming(guid)` again — full reconnect sequence.

Previously the driver recovered the bus but left audio streaming dead.

---

### Fix 5 — rescanAttempts_ accumulation on bus reset
**File:** `ASFWDriver/Protocols/AVC/AVCDiscovery.cpp`

`rescanAttempts_[guid]` counter was never reset on `OnUnitResumed`.
After N bus resets the device permanently fell out of discovery.

**Fix:** reset counter on resume.

---

### Fix 6 — IOPCIClassMatch instead of IOPCIMatch
**File:** `ASFWDriver/Info.plist`

`IOPCIMatch: 0x590111c1` (Agere chip only) →
`IOPCIClassMatch: 0x0c001000&0xffffff00` (any OHCI FireWire controller).

Now matches Apple TB → FW adapter (TI XIO2213B) and other OHCI chips.

---

### Fix 7 — RX queue wiring in StartDevice
**File:** `ASFWDriver/Isoch/Audio/ASFWAudioDriver.cpp`

`CreateRxQueue` in `ASFWAudioNub` is lazy — only called on first `StartAudioStreaming`.
`MapRxQueueFromNub` in `ASFWAudioDriver::Start` failed because the queue didn't exist yet
→ `rxQueueValid = false` → `ZtsTimerOccurred` skipped RX path → silence from FireWire IR.

**Fix:** after `nub->StartAudioStreaming()`, if `!rxQueueValid`, call `MapRxQueueFromNub`
again. Updates `inputChannelCount` from queue header.

---

### Fix 8 — TX queue wiring in StartDevice
**File:** `ASFWDriver/Isoch/Audio/ASFWAudioDriver.cpp`

Same lazy-init problem for TX queue. `HandleWriteEnd` was writing CoreAudio data
into a local ring buffer instead of the shared TX queue → no audio transmitted to device.

**Fix:** if `!txQueueValid`, call `MapTxQueueFromNub` after `StartAudioStreaming`.
Updates `outputChannelCount` from queue header.

---

### Fix 9 — Runtime sample rate switching via AV/C opcode 0x19
**Files:** `ASFWDriver/Isoch/Audio/ASFWIOUserAudioDevice.iig/.cpp`,
`ASFWDriver/Protocols/AVC/IAVCDiscovery.hpp/.cpp`,
`ASFWDriver/Protocols/AVC/AVCDiscovery.hpp/.cpp`

`IOUserAudioDevice` was created as a plain instance — `HandleChangeSampleRate` could
not be overridden. HAL rate changes were silently ignored; device was locked at 48 kHz.

**Fix:**
- **`ASFWIOUserAudioDevice`** — new `IOUserAudioDevice` subclass (`.iig` + `.cpp`)
  - `HandleChangeSampleRate(double)` override:
    1. `AudioCoordinator::StopStreaming(guid)`
    2. `SendSampleRateCommand(guid, rateHz, cb)` — AV/C INPUT PLUG SIGNAL FORMAT
       (opcode 0x19), SFC per IEC 61883-6 Table 5, poll ≤ 500 ms
    3. `SetSampleRate(rate)` — confirm new rate to CoreAudio HAL
    4. `AudioCoordinator::StartStreaming(guid)` — restart IR+IT at new rate
- **`IAVCDiscovery::SendSampleRateCommand`** — new virtual method on discovery interface
- **`AVCDiscovery::SendSampleRateCommand`** — implementation: lookup AVCUnit by GUID,
  build AV/C CDB, submit via FCPTransport, callback with accept/reject result
- **`ASFWAudioDriver`** — creates `ASFWIOUserAudioDevice` instead of `IOUserAudioDevice`

SFC mapping (IEC 61883-6 Table 5):
`32k=0x00 · 44.1k=0x01 · 48k=0x02 · 88.2k=0x03 · 96k=0x04 · 176.4k=0x05 · 192k=0x06`

---

## MOTU 828 MK3 Bring-Up Path (after all fixes)

| Step | Status |
|------|--------|
| OHCI init, bus reset, topology | ✅ |
| Config ROM scan | ✅ |
| AV/C discovery, Music Subunit | ✅ |
| IRM: AllocateResources | ✅ |
| CMP ConnectOPCR with channel | ✅ |
| oPCR read-back verification | ✅ |
| Bus reset recovery | ✅ |
| IOPCIClassMatch (TB adapter) | ✅ |
| RX queue wiring | ✅ |
| TX queue wiring | ✅ |
| Runtime sample rate switching | ✅ |
| **Hardware validation on Tahoe** | ⏳ pending |

---

## Notes

- All changes are backward-compatible with the Apogee Duet 2 path
- No existing tests were broken; 59 new tests added across all fix areas
- Hardware test planned: Mac Studio (Apple Silicon, macOS Tahoe) + TB→FW adapter + MOTU 828 MK3
