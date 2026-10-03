# MOTU 828 Mk3 (protocol v3)

**Status:** plays and records at 48 kHz on real hardware; every other rate and the
optical banks are gated off. This document describes what the series adds, how it
fits the existing audio architecture, and exactly what has and has not been
verified on a device.

## 1. What is verified, and on what

Hardware: one MOTU 828 Mk3 FireWire on an Apple Silicon MacBook (M3), two-node
bus, macOS 27.0.1, built with Xcode 27.0. All results below are **48 kHz
only**. "Verified" means checked on the 828 itself (front-panel meters and
listening) or read from the driver's log ring during a run, never inferred from
host tests alone.

| Area | Result |
|---|---|
| Identity | The 828 Mk3 is matched by its unit directory (`Unit_Sw_Version 0x15`); its root directory carries no `Model_Id`. |
| Playback | Clean audio on Main Out L/R, confirmed by meters and listening, in runs of up to ~30 min. An intermittent misframe seen in earlier builds has not reappeared, but is not ruled out. |
| Capture | Analog inputs 1–8 recorded in Logic Pro. |
| Lifecycle | Stop → start → stop through CoreAudio, repeated, clean. |
| SPH clock servo | Locks within one update after start (≈6 ms of muted output), stays locked, median phase error 0 ticks at the setpoint. |
| Telemetry | `[MotuSphServo]`, `[MotuSphState]`, `[MotuSphRef]`, `[MotuSphDrop]` every 2 s, `[RxSphRate]`/`[RxPhaseRel]` every 4 s, no dropped ring records. |

The playback, capture and lifecycle rows were re-checked on this series'
driver code on top of 0.4.0-beta.2: two full device sessions, the second
about nine minutes, the servo locked throughout, no hard resync, no replay
reset and no dropped log records.

Not yet verified on hardware:

- Any rate other than 48 kHz, and changing the rate from the driver. The rate
  index is read correctly from `CLOCK_STATUS`; the driver never writes it.
- The optical banks (more than 14 playback / 18 capture channels).
- The hard-resync action. The valve is reachable and its path is instrumented
  (no event fired in long runs), but provoking it interrupts audio and has not
  been done.
- The restart after a device buffer fault, end to end through CoreAudio. The
  status word is decoded and the restart is requested (host tests pin this); the
  CoreAudio half has not been observed on the device.

## 2. How the 828 Mk3 differs from protocol v2

The v3 data block looks like v2 — an SPH quadlet, two 24-bit message chunks,
then packed 24-bit PCM — but the timing model is different:

- **Capture header.** The 828 Mk3 puts a fixed 8-byte header where a CIP header
  would be (`0d 04 04 00 22 ff ff ff`). It is not an IEC 61883 CIP header (EOH1
  clear), so it is matched byte for byte and nothing in it is interpreted.
- **SYT is always `0xFFFF`.** NO-DATA is recognised by a packet without data
  blocks, not by SYT.
- **SPH is a one-second timestamp.** Cycle in bits 24:12, offset in 11:0, and
  the cycle timer's `seconds` field (31:25) left at zero — observed in every data
  packet, both directions, in a bus capture of the official driver. Everything
  derived from the SPH therefore wraps once per second.
- **Transmit SPH is a free-running clock, not a replay.** v2 replays the
  device's own presentation offsets. v3 transmits from a fractional (Q32.32)
  accumulator steered by a servo against the device's receive SPH.
- **Playback CIP.** FMT `0x02` and FDF `0x22` against the 48 kHz AM824 SFC,
  SYT `NO_INFO`, in both DATA and NO-DATA packets.

## 3. Architecture

The 828 Mk3 runs on the existing seams; no generic layer gained a MOTU branch.

```
Catalog            AudioDeviceCatalog row (unit directory match) -> MotuV3
Profile            MOTU828Mk3Profile: geometry, CIP fields, latencies, 48 kHz pin
Protocol           MOTU828Mk3Protocol on FamilyDriver: register choreography over
                   async register IO (MOTU828Mk3RegisterIO / RegisterModel)
Backend            MotuAudioBackend: v2 and v3; device notifications routed by
                   family (DeviceNotificationDispatch), status word -> restart
Receive            kMotuV3Header framing + the v2 payload codec (identical block
                   layout and DBS formula) + MotuV3RxTimingObserver
Transmit           shared MotuPayloadWriter under the Mk3 port map (MotuV3PayloadWriter)
                   + MotuV3TxTimingStamper behind ITxDeviceTimingStamper
Clock              MotuSphClockServo + MotuSphHardResyncGate + MotuRelPhaseConditioner
```

### 3.1 The RX → TX clock bridge

`MotuV3RxTimingObserver` measures the device's sample period from the slope of
the receive SPH (`MotuRxSphRateMeter`) and publishes the cumulative
frames/ticks over a generation-checked latest-value bridge in the transport
control block (`motuRxSphClock`). Once per four-second window it also forms
`rel`, the TX presentation phase against the RX absolute phase, conditions it
(`MotuRelPhaseConditioner`) and publishes it over a second bridge
(`motuRelPhase`).

The TX producer feeds both into the stamper **before every packet, NO-DATA
included**, on the TX preparation queue. Re-anchoring rules:

- an internal IR recovery or a replay discontinuity re-anchors the meter but
  keeps the cumulative clock monotonic within the stream generation;
- only a new control generation (StartIO) clears it.

### 3.2 The servo

`MotuSphClockServo` is a pure controller (no wire, no runtime, fully covered by
host tests):

- **Feed-forward:** the measured RX period.
- **Proportional:** the phase error against a measured setpoint
  (`kOracleRelSetpointTicks` = +510 ticks, measured off the official driver),
  normalised to the interval that actually elapsed.
- **Two stages, one loop:** gain 1/D while acquiring, 1/(4·D) once locked. The
  output is muted until the first decision under the lock threshold (1/50 of a
  bus cycle, 61.44 ticks), which takes one update.
- **Absolute seed:** a re-reference seeds the loop with the measured offset
  instead of declaring it zero, folded about the running operating point so the
  modulo-one-cycle branch cut stays half a cycle away from it.
- **Hard-resync valve:** above four bus cycles of error the loop returns to the
  measured rate and asks for a one-shot phase repair; only the valve drops the
  lock.

Every way an observation can be discarded before it reaches the valve is
counted and attributed (`[MotuSphRef]`, `[MotuSphDrop]`), so "no hard resync"
in a log is a measurement, not an absence of evidence.

### 3.3 Other changes outside the MOTU directories

- `IsochReceiveContext` sets `kWake` when starting the IR context; without it
  the context arms but never fetches (observed on the 828 Mk3).
- `ForceInitialBusReset` resets the bus unconditionally; gating it on
  `programPhyEnable` left an 828 Mk3 unseen after a driver restart.
- `DuplexIRMAdvisory`: on a bus with no IRM the reservation is advisory for
  device-assigned channels. The official driver allocates nothing on an ordinary
  playback start, and requiring the reservation failed every StartIO on a bare
  two-node bus.
- `AudioWireFormat::kMotuV3Packed` has its own value, distinct from `kMotuV2`,
  so v3 never takes a branch written for v2.

## 4. Diagnostics

- **Stream metrics** (`ASFWAudioStreamMetricsABI.h`, selector 1015): a
  synchronized snapshot of the transport counters, read by the app and exposed
  over MCP.
- **Isoch oracle capture** (`ASFWIsochOracleCaptureABI.h`, selector 1016): a
  bounded capture of the first packets of a stream start on both directions
  (DATA/NO-DATA, length, DBC, first SPH, OHCI cycle), for offline comparison
  against a capture of the official driver.
- **`[RxInputMeter]`:** per-channel peaks of what RX wrote into the input
  buffer, after the capture channel map — splits "the device sends silence" from
  "the application receives silence". `tools/motu_input_meter.swift` is the
  application-side counterpart.
- **`[TxChunkMap]` and the TX silence diagnostics:** where host→device silence
  comes from — the host, the servo mute, the placement or the packet.

## 5. Evidence and references

- **Wire behaviour** comes from passive bus captures of the official MOTU driver
  on OS X 10.11 and from the driver's own log ring on the device. The SPH parity
  test (`MotuV3SphWireParityTests`) pins raw quadlets from such a capture.
- **Register choreography and the status-word map** follow the official driver
  (`FWA_BoxWithFxDSP`, the class `FWA_Box828mk3` derives from), cross-checked
  against the device firmware. Where that differs from Linux `motu-stream.c`
  (the low 16 bits of `ISOC_COMM_CONTROL`), the official driver wins.
- Linux ALSA/FFADO were used only as public protocol structure, never as code.

## 6. Next steps

1. Run this series on the 828 Mk3 (playback, capture, stop/start).
2. Enable the optical banks behind a host-tested bank gate.
3. One rate per family (44.1 kHz, 96 kHz, 192 kHz), starting with rate
   switching from the driver.
4. Provoke the hard-resync valve and the buffer-fault restart on hardware.
