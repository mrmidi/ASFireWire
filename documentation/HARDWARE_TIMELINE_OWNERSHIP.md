# Hardware sample timeline: ownership (Epic 4)

**Status:** implemented, 2026-09-26 (T0–T7 on `refactor/hardware-timeline`). Hardware check
passed on the Saffire Pro 24 DSP at 48 and 44.1 kHz, the Apogee Duet and the M-Audio 1814 (§6). Linear FW-186 (milestone 4), with FW-187/188/189
condensed here. Stages T0–T7 in the milestone plan; this document is T0.

**Goal.** One authority for `absolute audio frame ↔ bus time ↔ host time`. CoreAudio's zero
timestamp (ZTS) is a *projection* of it, not a separate clock.

**Scope decisions (user, 2026-09-26):**
- **TX presentation planning (FW-194) moves to milestone 6.** The TX frame cursor keeps its
  hardware-validated one-shot RX-replay alignment.
- **Presentation loss starts a new epoch on the same source.** The RX→TX source switch is
  deferred to milestone 6.

## 1. Baseline (the quality to keep)

Instruments, Audio Statistics → Zero Time Stamp, Saffire Pro 24 DSP at 48 kHz, current build
(`8fa472e` + `557d4965`), 4.4 s, healthy stream:

| anchors | sample times | spacing | jitter min / max | jitter σ |
|---|---|---|---|---|
| 17 | 5,332,992 → 5,529,600 | 12,288 each (one period) | 3.08 µs / 3.33 µs | 79 ns |

Steady-state anchors are already clean. The defects are all about **discontinuities**
(§3), so the new path must reproduce these anchors exactly on a clean stream (T1 goldens).

## 2. Inventory and dispositions

Surveyed 2026-09-26 on `refactor/dice-profile`.

| Surface | Where | What it is | Disposition |
|---|---|---|---|
| RX ZTS publication (`absoluteFrameCursor_` + arrival host time, publishes when `firstFrame % period == 0`) | `Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp` | **Clock authority** for every non-M-Audio device | **REPLACE → `HardwareSampleTimeline` (Receive)** (T4). The cursor remains only as the input-buffer write position, derived from the timeline frame |
| M-Audio TX clock (`TxClockBridge`, private `HardwareSampleTimeline`, direct `UpdateCurrentZeroTimestamp`) | `Audio/Families/BeBoB/MAudio/MAudioTxClockBridge.*`, `ASFWAudioDriverZts.cpp` ~L93-235 | **Clock authority** for M-Audio special firmware | **ADAPT** (T3): uses the shared timeline (Transmit) and the common publish path |
| `HardwareSampleTimeline` | `Audio/Runtime/HardwareSampleTimeline.hpp` | Epoch-tagged frame↔bus↔host authority | **KEEP**: becomes the single authority, one instance per device in the transport control block |
| `HostClockAnchor` (+ `AudioClockPublisher`) | `Audio/Runtime/HostClockAnchor.hpp`, `Audio/Engine/Direct/AudioClockPublisher.*` | Cross-queue mailbox of the latest anchor | **ADAPT** (T3): carries the epoch; stale-epoch anchors are refused at publication |
| `PublishSharedZeroTimestampToHAL`, `lastHalZeroTimestamp*` | `ASFWAudioDriverZts.cpp` ~L34-89, `ASFWAudioDriverPrivate.hpp` | The one call into `UpdateCurrentZeroTimestamp`, plus its mirror | **KEEP** as the only HAL publish point (the M-Audio direct call folds into it) |
| `RxSytCadence` | `Audio/Wire/AMDTP/RxSytCadence.hpp` | SYT-delta history; "timing established" gate | **KEEP** as a gate and observation only |
| `RxSequenceReplay`, `ComputeReplaySyt*` | control block, `ASFWAudioDriverZts.cpp` | RX packet relay to TX (SYT, frames) | **KEEP**; TX-side use → milestone 6 |
| `DextTxExecutionTimeline` | `ASFWAudioDriverPrivate.hpp` | Extrapolates *bus* time from IT completions | **KEEP**: a transport execution observation, not sample time |
| TX frame cursor (one-shot align from the RX replay delta) | `ASFWAudioDriverZts.cpp`, `DiceTxStreamEngine` | Half-independent TX frame origin | **DEFER → milestone 6 (FW-194)** |
| `AmdtpPacketTimeline` | `Audio/Wire/AMDTP/AmdtpPacketTimeline.*` | Prepared-packet metadata | **KEEP**: metadata (and the M-Audio TX frame source) |
| `MotuRxTiming`, `IRxDeviceTimingObserver` | `Audio/Wire/MOTU/*` | Device-specific establishment gate | **KEEP** as a gate |
| `ZtsTelemetry` | `Isoch/Receive/ZtsTelemetry.hpp` | Log ring of publishes | **KEEP**; publishes also record the epoch |
| `DeviceTimeline` | `Audio/DriverKit/Runtime/DeviceTimeline.hpp` | No production writer | **DELETE** (T2) |
| `rxDbcFrameCount` | control block | No reader | **DELETE** (T2) |
| `ICycleTimeline`, `SessionClock` | `Audio/Ports`, `Audio/Session` | Scheduling "now", not sample time | **KEEP** (out of scope) |
| Start-up anchor timeout (`initialClockAnchorTimeoutMs`) | `ASFWAudioDevice.cpp` | Wait for the first real anchor; no synthetic seed | **KEEP** |
| HAL clock algorithm (IIR default vs Raw) | `SetClockAlgorithm` | CoreAudio-side smoothing | **DEFER**: decided from baseline data (FW-176), not assumed |

## 3. Target model

```
RX packet arrival (receive cycle timer + drain host/cycle pair)     M-Audio: TX completion stamps
                    │  observation (Receive)                                 │  observation (Transmit)
                    └──────────────►  HardwareSampleTimeline  ◄──────────────┘
                                   (one per device, epoch-tagged)
                                        │ boundary (sampleFrame, hostTicks, epoch)
                                        ▼
                         HostClockAnchor → PublishSharedZeroTimestampToHAL → HAL
```

- **Source, chosen at epoch start:**
  - Receive by default;
  - Transmit when the profile's `TransmitClockSource` is `kInternalCadence` (M-Audio
    special firmware) or the device has no inputs.
- **One instance:** in the audio transport control block, beside `hostClockAnchor`. The
  control block's lifetime is already the seam contract (FW-60), so no new cross-service
  pointer is introduced.

### Decision (a): observation basis stays **arrival**

The current anchor is `host = drainHost − age(packet cycle → drain)`, which is the packet's
arrival. It does not read SYT, so it works unchanged for devices without SYT (RME
`CIP_NO_HEADER`, MOTU SPH; memory: "ZTS must not depend on CIP fields").

The timeline receives:
- `presentationBusTicks` = the packet's receive time as unwrapped bus ticks;
- the correlation pair = the batch drain's `(bus, host)`.

`ProjectBusToHost` then reproduces today's host time exactly. So a clean stream's anchors
stay **byte-identical** (T1 → T4), preserving the §1 jitter.

Moving to SYT presentation, as `midi` does, would be a separate declared delta and needs
hardware comparison. It is not part of this epic.

### Decision (b): loss starts a new epoch at the current frame

- **Today:** a receive-cycle gap resets cadence and replay and fires the timing-loss
  callback, which restarts the stream. The frame cursor keeps counting decoded frames
  across the gap, so any anchor published before that restart is short by the lost
  frames. The goldens pin it: +125 µs after one lost packet, +1 ms after an 8-cycle gap.
- **Rule (T4):** an established stream's discontinuity (`kReceiveCycleGap`, a rejected
  cadence or anchor) begins a `PresentationLoss` Receive epoch **at the current frame**.
  The old mapping is dropped, and an anchor projected before the gap is refused at the HAL
  (`StaleEpoch`).
- **Frames are not renumbered.** `midi`'s rule, a loss base at `NextBoundary(last)`, was
  considered and rejected here:
  - it would move the input-ring write position by up to one period;
  - it would move the RX replay frames, which the TX cursor re-aligns from after a stall.
    That is milestone 6's TX-ownership territory.
  - It also buys nothing today: the timing-loss restart that follows (StopIO → StartIO
    through CoreAudio, `557d4965`) begins a fresh StartIO epoch anyway.
- **Deferred to FW-218:** accounting for the lost frames (by DBC where present, or by bus
  time) so a stream can survive a loss *without* restarting. That goes with the
  restart-versus-re-anchor decision. References that re-anchor instead of restarting:
  - Saffire.kext re-anchors every DCL group from the interrupt host time;
  - AppleFWAudio re-anchors at every ring wrap from a fresh cycle-timer read.

### Epoch triggers

| Reason | Where it starts |
|---|---|
| `StartIO` | `ASFWAudioDevice::StartIO` (every start, including CoreAudio's handover restart) |
| `SampleRate` | covered by StartIO: a rate change commits inside a configuration-change window, and the host's StartIO that follows begins the new epoch |
| `BusGeneration` | covered by StartIO: a restart while CoreAudio runs goes through StopIO → StartIO (`557d4965`) |
| `PresentationLoss` | the RX consumer's discontinuity detection, at the current frame (decision b) |
| `ClockSource`, `HardwareRestart`, `SourceSwitch` | not used in milestone 4 (the source switch is deferred) |

Consumers never carry a mapping across an epoch: `Observe` rejects stale-epoch
observations, and `HostClockAnchor` refuses stale-epoch anchors.

## 4. Deferred

- **FW-194:** TX presentation planning through `PreviewTxRange`/`CommitTxRange` → milestone 6 (FW-209).
- **The RX→TX source switch on presentation loss** → milestone 6.
- **SYT-presentation basis** instead of arrival: needs hardware comparison; not in this epic.
- **HAL clock algorithm:** from FW-176 baseline data.
- **Whether a timing loss restarts the stream or only re-anchors:** FW-218.

## 5. Stages

| Stage | Work |
|---|---|
| T1 | Golden anchors for the RX path (`tests/golden/zts/`) |
| T2 | Delete `DeviceTimeline`, `rxDbcFrameCount` |
| T3 | Shared timeline, epochs, epoch-tagged `HostClockAnchor`, M-Audio on the shared instance |
| T4 | RX feeds the timeline (arrival basis, in-packet boundaries, decision b) |
| T5 | 44.1/88.2 `NominalBusTicksPerFrame` gap (G-17) |
| T6 | Split `ASFWAudioDriverZts.cpp` into ZTS publication and the TX producer |
| T7 | Reverse audit and docs |

## 6. Final state and reverse audit (T7)

The final tree, searched:

| Question | Answer |
|---|---|
| Who calls `UpdateCurrentZeroTimestamp`? | **One place:** `PublishSharedZeroTimestampToHAL` (`ASFWAudioDriverZts.cpp`) |
| Who writes the anchor mailbox? | The RX publisher (`AudioClockPublisher`, Receive epochs only) and the M-Audio TX clock (`ObserveMAudioTxClock`, Transmit epoch). Both carry the timeline epoch |
| How many `HardwareSampleTimeline` instances? | **One**, `AudioTransportControlBlock::hardwareTimeline` |
| Who begins an epoch? | StartIO (`SelectTxClockDomain`: Receive; `TxClockBridge::Arm`: Transmit) and the RX consumer on presentation loss |
| Who writes the HAL mirror (`lastHalZeroTimestamp*`)? | The publish function; everything else is a reset at bind or start |
| Is a stale mapping ever published? | No: `Observe` rejects stale-epoch observations, and the publish point refuses stale-epoch anchors (`StaleEpoch`) |
| Rates | Every HAL-ladder rate has an epoch (`HalRateTier`); outside the ladder, the old grid rule remains as the only compatibility path |

**Rows resolved, per §2:**
- **REPLACE:** the RX authority now observes, and the timeline projects.
- **ADAPT:** the M-Audio clock uses the shared instance and the common path.
- **ADAPT:** `HostClockAnchor` carries the epoch.
- **DELETE:** `DeviceTimeline`, `rxDbcFrameCount`.
- **KEEP:** the observations and gates.
- **DEFER:** the rest, below.

**Still deferred, with owners:**
- **FW-194**, TX presentation planning, and the TX frame cursor's one-shot RX-replay alignment → milestone 6 (FW-209).
- **The RX→TX source switch on presentation loss** → milestone 6.
- **Accounting for lost frames so a stream survives a loss without a restart** → FW-218.
- **A Transmit clock for playback-only devices without the M-Audio bridge** (e.g. Weiss). Only the M-Audio bridge feeds Transmit observations today; any other device stays on Receive → milestone 6 (TX completion observations).
- **SYT-presentation basis instead of arrival** → needs a hardware comparison.
- **HAL clock algorithm (Raw vs IIR)** → from the FW-176 baseline.

**Hardware check, 2026-09-26: passed** (Saffire Pro 24 DSP, 48 kHz, cold start, build
`79a0737`):

| | before (`8fa472e`) | after (`79a0737`) |
|---|---|---|
| Instruments ZTS anchors | 17 | 12 |
| spacing | 12,288 each | 135,168 / 11 = 12,288 each |
| jitter min / max | 3.08 / 3.33 µs | 3.08 / 3.25 µs |
| jitter σ | 79 ns | 50 ns |

The MCP driver ring agrees:
- one `epoch=1 source=receive reason=start-io`;
- the first ZTS at frame 12288;
- no `StaleEpoch`, no new warning types, 0 dropped records;
- 17 logged anchors fit a straight line within −1.45…+2.12 µs (RMS 0.89 µs), a −12.1 ppm device clock.

"Not worse" is the claim. The clean path is byte-identical by construction, and the σ
difference is within the sample size.

**Bus-reset burst while playing, 2026-09-26: recovered** (Pro 24 DSP, 48 kHz, build
`6a74bbf`, three local resets about 0.5–2 s apart over MCP):
1. The RX loss begins a `presentation-loss` epoch at the current frame.
2. Each suspension cancels the pending quiet-period restart.
3. After the last resume, the restart is handed to CoreAudio: StopIO (`0x0`), then StartIO.
4. The new start begins a fresh Receive epoch, and the first ZTS lands at frame 12288.

No `Prime failed`, no `IT FATAL`, no stale anchor, and audio came back without touching
playback. This also verifies the FW-218 recovery fixes (`557d4965`, `3e31eab1`), which had
failed twice before them.

**48 → 44.1 kHz switch while playing, 2026-09-26: passed** (Pro 24 DSP, same build):
- The device locks at 44.1 kHz 80 ms after the request. The restart logs
  `[Zts] epoch=1 source=receive reason=start-io rate=44100`.
- The first ZTS lands at frame 12288, and every later one is an exact multiple of 12288.
- No RX replay reset, `StaleEpoch`, loss epoch or `Prime failed`. 0 dropped records.
- Playback sounds clean, by ear better than at 48 kHz, where a few glitches were heard.
  Not investigated: parked until the audio stack is complete.

The clock fit is noisier than at 48 kHz: 12 anchors over 44 s fit a +11.1 ppm line with
RMS 31.7 µs (±54 µs). At 48 kHz it is 0.89 µs. This follows from decision (a), and the old
path behaved the same way:
- A ZTS host time is the arrival time of the packet that carries the boundary frame.
  Arrival is only known to one bus cycle (125 µs).
- At 48 kHz, 12,288 frames are exactly 2,048 cycles, so that packet arrives at the same
  point in a cycle every time.
- At 44.1 kHz they are 2,229.14 cycles, so the point walks through the cycle. An error
  spread evenly over 125 µs gives about 36 µs RMS.
- ±54 µs is about 5 % of the HAL's 1 ms out-of-bounds gate, so the HAL filters these
  anchors; it does not substitute them.
- The SYT-presentation basis (§4) would remove it.

Also done: M-Audio 1814, cold start, clean after `6a74bbf6` (see that commit); Apogee Duet
(Receive epoch) clean; hot swaps 1814 ↔ DICE clean.
