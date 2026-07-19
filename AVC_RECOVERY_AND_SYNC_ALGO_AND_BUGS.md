# AV/C recovery and duplex SYT: algorithm, evidence, and bugs

Status: design/incident note, 2026-07-18.  This is deliberately a behavioural
cross-reference, not source reuse.  Linux is GPL and FFADO is LGPL; neither is
copied into ASFW.

## Executive conclusion

The Duet's RX and TX SYT fields must **not** be compared as equal 16-bit
numbers.  They are timestamps for two independently packetised directions,
each with its own empty-packet placement, transfer delay, packet-to-frame
mapping, and device buffering.  A fixed *or patterned* phase difference can be
normal.  A fault is a discontinuity, an implausible per-direction cadence, or
a changing phase after each direction has been mapped into the same unwrapped
FireWire time domain and its own transfer delay has been accounted for.

The demonstrated ASFW failure is separate and concrete: its AV/C timing-loss
recovery restarts the transport without asking the audio-side producer to reset
and prefill its shared TX queue.  The old live `committedEnd` is then passed to
IT prime as the initial prefill count.  It is far beyond the 408-slot ring and
prime rejects the restart.  That explains why the recovery path dies; it does
not yet identify the original RX timing loss after several minutes.

The target is **continuous playback for days**, not a restart which happens to
sound acceptable.  Recovery is a last-resort response to a proven xrun, bus
reset, unplug, or clock-source change.  It must never be used to paper over a
local SYT, descriptor, or queue-accounting defect.

## Evidence used

| Evidence | What it establishes | Limits |
| --- | --- | --- |
| Duet driver-ring incident (the `AUDIO DUPLEX START` log supplied with this task) | The first stream start succeeds; later timing-loss recovery reaches `IT: Prime failed - committed prefill=13626402 must cover 48 descriptors within 408 slots`, then returns `0xe00002c9`. | It does not say why RX cadence initially stopped. |
| Apple FireBug 2.3 trace supplied on 2026-07-18 (not committed) | The Duet emits on channels 0 and 1 every isochronous cycle, with 8-byte empty/no-info and 72-byte audio packets in each direction.  The capture reports 0 silent cycles on both active channels. | A FireBug packet trace alone does not label host-to-device versus device-to-host; map that through the CMP connection record. |
| AppleFWAudio static inspection via the attached IDA MCP instance | Apple stops the direction, prepares/rebuilds stream state, reconnects it, then starts it; it is not a transport-only arm.  Its AM824 writer also maintains a 400-cycle (48 kHz) content horizon inside an 800-cycle default ring. | Decompiler field names and inferred structs are not a contract.  Do not copy Apple code. |
| `references/linux-sound-firewire-stack` | Current Linux AMDTP/BeBoB/OXFW behaviour: separate direction state, SYT/DBC validation, no-data handling, restart/replay tolerances. | Linux implementation and naming are not an ASFW architecture template. |
| `references/libffado-2.5.0/src` | FFADO keeps full per-direction timestamps, turns an RX SYT into full ticks, and uses a TX transfer delay plus a prebuffered timeline. | FFADO uses a different userspace/raw1394 mechanism. |

## What the FireBug capture says

At the beginning of the capture, channel 1 has a 72-byte data packet with
`SYT=0xa31a`; channel 0 has an 8-byte packet with `SYT=0xffff`.  In the next
cycle the roles change: channel 1 is empty and channel 0 carries data with
`SYT=0xa71a`.  This continues with each direction independently interleaving
72-byte audio packets and 8-byte packets:

```text
cycle 7632: channel 1 data    SYT a31a; channel 0 empty   SYT ffff
cycle 7633: channel 1 empty   SYT ffff; channel 0 data    SYT a71a
cycle 7634: channel 1 data    SYT b71a; channel 0 data    SYT bb1a
```

The concrete packets are at capture lines 4-31.  Its end counters (lines
503-509) show one packet per cycle, 0 silent cycles, and the same approximately
6:38 active duration for both channels.

Two important details:

1. `0xffff` is **no presentation timestamp**.  In this trace it accompanies
   an 8-byte CIP-only packet.  Do not use it as a clock observation.  Also do
   not require `FDF == 0xff` to identify an empty packet: this capture has the
   active FDF sample-rate value (`0x02`) with `SYT=0xffff` and no payload.
2. The FireBug `seconds:cycle:offset` at the left is packet observation time.
   CIP SYT is a separate, 16-cycle-modulo presentation time.  Raw subtraction
   across channels loses wrap information and confuses packet scheduling with
   the time at which a device presents samples.

Therefore the capture is evidence *against* an assumption that TX SYT must
equal RX SYT.  It is **not**, by itself, proof that either direction is out of
sync.

## Cross-reference: the reference stacks agree

### Linux AMDTP/AV-C family

Linux treats AMDTP direction state as independent:

- A blocking stream includes empty/NODATA packets; a non-blocking stream has a
  variable number of events per packet
  ([`amdtp-stream.c:213-240`](references/linux-sound-firewire-stack/amdtp-stream.c#L213-L240)).
  At 48 kHz, blocking packets are aligned to the 8-frame SYT interval, not to
  “one audio packet every cycle”
  ([`amdtp-stream.c:236-240`](references/linux-sound-firewire-stack/amdtp-stream.c#L236-L240)).
- It gives a blocking direction an *additional direction-local transfer
  delay* to accommodate no-data packets
  ([`amdtp-stream.c:271-296`](references/linux-sound-firewire-stack/amdtp-stream.c#L271-L296)).
  This alone makes raw RX/TX SYT equality an invalid invariant.
- On receive it checks DBC continuity and separately extracts SYT
  ([`amdtp-stream.c:772-810`](references/linux-sound-firewire-stack/amdtp-stream.c#L772-L810));
  it rejects discontinuous cycle sequences rather than treating a different
  opposite-direction SYT as an error
  ([`amdtp-stream.c:935-1001`](references/linux-sound-firewire-stack/amdtp-stream.c#L935-L1001)).
- On transmit it computes each direction's SYT from that packet's output cycle
  plus a per-stream sequence offset and transfer delay
  ([`amdtp-stream.c:1004-1057`](references/linux-sound-firewire-stack/amdtp-stream.c#L1004-L1057)).
  It resets its packet/DBC/sequence state when creating the stream, before it
  is armed ([`amdtp-stream.c:1666-1773`](references/linux-sound-firewire-stack/amdtp-stream.c#L1666-L1773)).

The device-family drivers reinforce that this is device policy, not a universal
SYT-equality rule:

- BeBoB starts both prepared streams, allows initial NODATA packets, and waits
  for valid SYT-bearing traffic before declaring the domain ready
  ([`bebob_stream.c:620-665`](references/linux-sound-firewire-stack/bebob/bebob_stream.c#L620-L665)).
- OXFW explicitly has models which ignore playback SYT; for those models the
  data-block sequence is the media-clock signal
  ([`oxfw-stream.c:360-390`](references/linux-sound-firewire-stack/oxfw/oxfw-stream.c#L360-L390)).
  Its quirk selection can mark a stream SYT-unaware
  ([`oxfw-stream.c:151-180`](references/linux-sound-firewire-stack/oxfw/oxfw-stream.c#L151-L180)).

The Duet is AV/C/CMP, not evidence that it uses an OXFW controller.  The OXFW
lesson is narrower: an AV/C backend must make device-specific timestamp policy
explicit; it cannot quietly promote raw cross-direction SYT equality to a
generic invariant.

### FFADO AMDTP

FFADO likewise keeps a full direction-local timeline:

- Its receive processor accepts only a valid CIP/SYT packet and converts SYT
  plus the packet cycle timer into full ticks
  ([`AmdtpReceiveStreamProcessor.cpp:97-123`](references/libffado-2.5.0/src/libstreaming/amdtp/AmdtpReceiveStreamProcessor.cpp#L97-L123)).
- Its transmit processor starts from the presentation timestamp of the next
  audio block, subtracts its own transmit transfer delay, and schedules the
  packet in a send window
  ([`AmdtpTransmitStreamProcessor.cpp:100-128`](references/libffado-2.5.0/src/libstreaming/amdtp/AmdtpTransmitStreamProcessor.cpp#L100-L128),
  [`:184-238`](references/libffado-2.5.0/src/libstreaming/amdtp/AmdtpTransmitStreamProcessor.cpp#L184-L238)).
- Its diagnostics validate the delta between *successive timestamps of one
  processor* against nominal rate; they do not compare RX and TX raw SYTs
  ([`StreamProcessor.cpp:463-490`](references/libffado-2.5.0/src/libstreaming/generic/StreamProcessor.cpp#L463-L490)).
- It initializes a transmit tail timestamp from the transfer time and
  prebuffer, then separately aligns received streams
  ([`StreamProcessorManager.cpp:840-879`](references/libffado-2.5.0/src/libstreaming/StreamProcessorManager.cpp#L840-L879)).

For a bus reset, FFADO locks out the manager's wait loop before the per-stream
handler runs ([`StreamProcessor.cpp:153-168`](references/libffado-2.5.0/src/libstreaming/generic/StreamProcessor.cpp#L153-L168)).
Its default handler declares an xrun/error, rather than pretending an existing
running producer queue can simply be re-armed
([`StreamProcessor.cpp:141-149`](references/libffado-2.5.0/src/libstreaming/generic/StreamProcessor.cpp#L141-L149)).

## Correct ASFW duplex timing model

ASFW already has the right *building blocks*, but they must remain distinct:

- RX consumes a valid CIP/SYT packet, observes cadence, stores its
  source-cycle/SYT/DBC sequence, and resets the replay epoch on a discontinuity
  ([`DirectAudioReceiveConsumer.cpp:206-236`](ASFWDriver/Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp#L206-L236),
  [`:297-310`](ASFWDriver/Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp#L297-L310)).
- `ComputeReplaySytOffset()` turns the RX 16-bit SYT into a delay-free offset
  relative to the received packet; `ComputeReplaySyt()` applies an output
  packet cycle and a TX transfer delay
  ([`RxSequenceReplay.hpp:26-92`](ASFWDriver/Audio/Wire/AMDTP/RxSequenceReplay.hpp#L26-L92)).
- The timing helpers correctly describe the 16-cycle SYT field and provide a
  full-cycle expansion which preserves seconds and handles a cycle-second
  boundary ([`TimingUtils.hpp:55-93`](ASFWDriver/Audio/Wire/AMDTP/TimingUtils.hpp#L55-L93),
  [`:178-200`](ASFWDriver/Audio/Wire/AMDTP/TimingUtils.hpp#L178-L200)).

The required algorithm is:

1. For **each direction**, reject a no-info SYT and retain `(packet cycle
   timer, SYT, DBC, data blocks, discontinuity)`.  Do not feed 8-byte/no-info
   packets into cadence estimation.
2. Expand a valid SYT using its own packet's full cycle timer into presentation
   ticks.  The 16-bit field alone is never a timeline.
3. Validate each direction locally: packet-cycle continuity, DBC progression,
   valid-SYT cadence, data-block cadence, and rate error.  At 48 kHz one sample
   is 512 FireWire ticks and a blocking SYT interval is eight frames/4096
   ticks; packet presence can still be interleaved with empty packets.
4. For TX, take a presentation epoch from the selected clock source, apply the
   **TX** transfer delay, then encode an SYT for the actual output packet
   cycle.  RX transfer delay and TX transfer delay are separate profile
   parameters.
5. Only for a duplex diagnostic, compare the two expanded presentation
   timelines after direction-local delays.  Track a robust phase distribution
   and drift over many valid packets.  Accept a stable device-specific
   phase/pattern; flag a step, a growing rate error, or a local cadence/DBC
   break.

This makes an explicit policy decision possible:

| Device policy | Clock source / validation |
| --- | --- |
| SYT-aware AV/C device (the current Duet hypothesis) | RX valid-SYT cadence establishes the device timeline; TX is generated from that timeline with its TX delay. |
| SYT-unaware playback device (an OXFW-style quirk) | Do not derive correctness from TX SYT.  Preserve the correct data-block cadence/DBC sequence and use the device's permitted no-info policy. |
| No valid RX timing source | Do not silently reuse stale replay state indefinitely.  Declare timing loss, stop safely, and enter a full producer+transport recovery epoch. |

## Confirmed recovery bug: stale TX producer cursor

### Reproduction chain

1. Normal `StartIO` explicitly runs the producer-owned
   `ResetProducerForStart()` before prefill and then validates that prefill
   exposed exactly `numSlots`
   ([`ASFWAudioDevice.cpp:221-245`](ASFWDriver/Audio/DriverKit/ASFWAudioDevice.cpp#L221-L245),
   [`:341-360`](ASFWDriver/Audio/DriverKit/ASFWAudioDevice.cpp#L341-L360)).
2. `committedEnd` is producer-to-consumer, while
   `ResetConsumerForArm()` intentionally does not alter that producer cursor
   ([`IsochTxQueue.hpp:132-155`](ASFWDriver/Isoch/Core/IsochTxQueue.hpp#L132-L155)).
3. IT `Start()` reads `committedEnd` and passes it as `preFillCount` to DMA
   prime ([`IsochTransmitContext.cpp:286-296`](ASFWDriver/Isoch/Transmit/IsochTransmitContext.cpp#L286-L296)).
4. Prime accepts only `48 <= preFillCount <= numSlots`; the live Duet incident
   supplied `13,626,402` for a 408-slot queue
   ([`IsochTxDmaRing.cpp:133-179`](ASFWDriver/Isoch/Transmit/IsochTxDmaRing.cpp#L133-L179)).
5. AV/C recovery calls `duplexCoordinator_.RecoverStreaming()` after RX replay
   has not recovered ([`AVCAudioBackend.cpp:231-288`](ASFWDriver/Audio/Protocols/Backends/AVCAudioBackend.cpp#L231-L288)).
   It does not pass through the normal audio producer reset+prefill sequence.

**Root cause:** recovery has consumer/transport authority but tries to restart
an audio-owned producer contract.  This violates the seam ownership rule even
though the cursor happens to be in shared memory.

### Required recovery transaction

Do not “fix” this by having `IsochTransmitContext` zero `committedEnd`: that
would race the audio producer and would move audio semantics into transport.
Instead add a generation/acknowledgement recovery protocol across the existing
neutral control block/ports seam:

```text
timing-loss verdict
  -> block new TX production at recovery epoch E
  -> quiesce RX/TX DMA and disconnect CMP in the profile-defined order
  -> audio producer resets packetizer, DBC/SYT/replay state and committedEnd
  -> audio producer writes and commits exactly numSlots safe prefill packets
  -> producer publishes PrefillReady(E, committedEnd == numSlots)
  -> transport validates E + committed slots, resets only consumer state, primes, arms
  -> reconnect/enable CMP, wait for valid RX cadence, release producer
```

All async callbacks must carry/check epoch `E`; a late refill, completion, or
timing-loss callback from the old session must be ignored.  The failure path
must leave the audio engine stopped/xrun, never a half-running CMP connection.
This mirrors the *behavioural ordering* seen in AppleFWAudio (stop direction →
prepare stream state → reconnect → start) and the ownership/reset discipline in
Linux and FFADO, without copying their mechanisms.

## Other known/suspected bugs

| ID | Status | Finding | Required action |
| --- | --- | --- | --- |
| `AVC-RECOVERY-001` | **Confirmed** | Stale `committedEnd` makes IT prime reject timing-loss recovery. | Implement the producer-owned recovery epoch above, then test a forced RX-cadence loss. |
| `AUDIO-RECOVERY-ROUTING-001` | **Confirmed critical defect** | FW-64 / merge `e9ea6ce` added a second registration to `IsochService`'s single `timingLossCallback_`. `AudioCoordinator` constructs `dice_` before `avc_`; AV/C therefore overwrites DICE's callback. A DICE/Focusrite replay reset is routed to AV/C, whose `activeGuid_` does not match and which drops the event. | Replace the single backend-owned callback with one AudioCoordinator-owned router which selects the backend by GUID. Carry the fault reason and session epoch; do not register directly from either backend. |
| `AVC-RECOVERY-002` | **Confirmed critical defect** | FW-64 treats every *established* replay reset as an eventual destructive `RecoverStreaming()`. A reset can mean a malformed RX packet, one cycle gap, rejected cadence, or rejected clock anchor—not necessarily a device outage. The coordinator stops/restarts transport while CoreAudio remains running with its old TX producer state. | Disable AV/C automatic transport restart until recovery atomically quiesces/resets/re-primes the audio producer and transport under one recovery epoch. Report the fault and request an audio-side xrun/controlled restart instead. |
| `AVC-PROPERTY-001` | **Fixed; requires hardware confirmation** | `AudioNubPublisher` sets stream mode on the nub, but `ASFWAudioDevice::PopulateNubProperties()` did not publish `ASFWStreamMode`; the driver parser consequently defaulted to non-blocking.  The model default is also non-blocking ([`ASFWAudioDevice.hpp:52-60`](ASFWDriver/Audio/Model/ASFWAudioDevice.hpp#L52-L60)); parsing only changes it if the property exists ([`AudioDriverConfig.cpp:67-84`](ASFWDriver/Audio/DriverKit/Config/AudioDriverConfig.cpp#L67-L84)).  The matching dictionary now publishes the selected mode. | On the next Duet/BeBoB start, confirm the AudioDriver reports `Stream mode from nub: blocking`; the property path is shared by all published audio nubs. |
| `AVC-TX-EXPOSURE-001` | **Confirmed audio-path defect; root scheduling cause open** | The payload writer skips CoreAudio frames which arrive beyond `Timeline().ExposedFrameEnd()` (`framesWithoutPacket`); `TxAlign` later repositions the producer cursor to close the deficit.  This can produce audible corruption while DMA/CMP and the TX descriptor lead remain healthy, then self-heal without a stream restart ([`AmdtpPayloadWriter.cpp:90-126`](ASFWDriver/Audio/Wire/AMDTP/AmdtpPayloadWriter.cpp#L90-L126), [`ASFWAudioDriverZts.cpp:360-389`](ASFWDriver/Audio/DriverKit/ASFWAudioDriverZts.cpp#L360-L389)). | Retain pending host frames until slots exist and maintain a data-bearing packet horizon beyond the maximum IO callback plus measured scheduling jitter; never discard a running stream's host frames because their packet is not yet exposed. |
| `TX-LAPLOSS-001` | **Withdrawn — the fix was the regression** | The timestamp-based lap-loss detector double-counted a stale completion baseline under coalesced interrupts and phantom-killed a healthy Saffire stream (all mod-48 deltas were < 48; FireBug showed 2.24 M continuous packets). It also treated a symptom of `TX-IRQ-001` and violated the transport boundary. To be reverted. | Revert the reconciliation; keep the watchdog-silence fatal + `ctrl`/`intEvent` snapshot. See "backend boundary leakage" below. |
| `AUDIO-BACKEND-BOUNDARY-001` | **Confirmed (Linux + Focusrite validated)** | AV/C resync machinery leaks into DICE: a single shared `IsochService::timingLossCallback_` (AV/C overwrites DICE) and cross-backend `RecoverStreaming`. Reference stacks keep recovery strictly per-family; the shared core is family-blind. | Sever the shared callback slot; make transport report-only state; re-home DICE recovery onto DICE notifications. |
| `TX-IRQ-001` | **Open** | The IT interrupt path went permanently silent ~10 minutes into the 2026-07-19 session; the refill watchdog carried the stream. Cause unknown (mask write, storm mitigation, dispatch loss). | Reproduce with the new watchdog-engagement log; correlate with `IsoXmitIntMask` writes. The 16-kick fatal now bounds the damage. |
| `TX-STALL-001` | **Open root cause; detection fixed 2026-07-19** | Saffire Pro 24 DSP (cycle master) run: both OHCI isoch contexts froze ~8 s at ~67 s while the wire, async, and host stayed healthy; no bus reset or `cycleInconsistent` fired. IR recovered alone; IT crawled 13 packets and died. Trigger unknown (cycle-start recognition, controller wedge, DMA stall). | Refill now faults `context-stalled` (untouched cursors) when unaccounted progress exceeds the committed lead, and records `ContextControl` + latched `IntEvent` at detection/watchdog time. Use those on the next occurrence. |
| `TX-FAULT-PROP-001` | **Confirmed** | After `IT FATAL STOP`/`[TxProducerFatal]`, nothing stops the ADK stream, notifies the backend, or releases CMP/IRM resources: CoreAudio runs IO against a dead stream indefinitely while the DICE backend keeps reporting the device healthy. | Propagate the transport/producer fault to a terminal, user-visible stream state (controlled stop/xrun) and release resources. Interim slice of the single-recovery-epoch design. |
| `AVC-SYNC-001` | **Not proven** | Treating raw RX/TX SYT inequality as a fault would be wrong for the captured duplex stream. | Add the normalized, per-direction trace below before adjusting delays or cadence. |
| `AVC-HEALTH-001` | **Instrumented; root cause open** | The original RX timing loss occurred without a bus reset in the observed driver ring; successful restart was then masked by `AVC-RECOVERY-001`. | Reproduce with the first-fault record below; it distinguishes a local RX validation failure from a real no-RX/CMP outage. |

### FW-64 callback overwrite and unsafe recovery escalation

Commit [`e9ea6ce`](https://github.com/mrmidi/ASFireWire/commit/e9ea6ce2fd60fc1e76efaf47fe9fa6feb3ddfaf4)
introduced AV/C timing-loss recovery. Its fault signal is global, but its
handlers are backend-specific:

```text
AudioCoordinator construction order
  dice_ constructor -> IsochService::SetTimingLossCallback(DICE handler)
  avc_ constructor  -> IsochService::SetTimingLossCallback(AV/C handler)
                         ^ overwrites the DICE handler

RX replay reset -> IsochService::OnReceiveTimingLossDetected(active GUID)
                -> AV/C handler only
                -> DICE GUID is not AV/C active GUID -> event dropped
```

This is a direct DICE stream-killer *after* an RX replay reset: the receive
side has discarded its timing history, but the DICE backend never receives the
recovery event. It does not itself create the initial RX reset. The independent
TX replay-horizon failure described below can also produce NODATA/clicks/silence
without any `[RxReplayReset]`; both failures must be fixed.

The AV/C path is unsafe in the opposite direction. An established replay reset
is raised for packet-processor failure, invalid RX timestamp, receive-cycle
gap, rejected SYT cadence, or rejected clock anchor
([`DirectAudioReceiveConsumer.cpp:178-336`](ASFWDriver/Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp#L178-L336)).
FW-64 waits 256 ms and, if the same replay has not re-established, calls
`RecoverStreaming()` ([`AVCAudioBackend.cpp:213-288`](ASFWDriver/Audio/Protocols/Backends/AVCAudioBackend.cpp#L213-L288)).
That operation stops and starts the duplex transport
([`AudioDuplexCoordinator.cpp:560-635`](ASFWDriver/Audio/Protocols/Backends/AudioDuplexCoordinator.cpp#L560-L635)),
but does not run the CoreAudio `StopIO`/`StartIO` producer reset and prefill
protocol. It can therefore turn a local timing anomaly into a stale-cursor,
half-running stream.

The repair boundary is architectural:

1. `AudioCoordinator` must own the only timing-loss subscription and dispatch
   by active GUID/backend; `IsochService` must not be a last-writer-wins
   backend callback slot.
2. The event must carry reset reason, RX epoch, and the active duplex session
   epoch so a delayed callback cannot act on a newer session.
3. Until the audio-side and transport-side recovery are one atomic epoch,
   AV/C timing loss is telemetry plus controlled xrun/explicit restart—not a
   background `RecoverStreaming()` call.
4. Callback replace/clear/invoke must be serialized with service teardown;
   the current unsynchronized `std::function` slot risks a callback into a
   destroying backend.

## Implemented first-fault MCP instrumentation

The driver log ring is the source of truth for this fault.  It retained all
13,480 records from the observed run with zero drops, but it previously did not
identify *which* RX validation path reset an established replay epoch.  That
gap is now closed by `[RxReplayReset]` in
[`DirectAudioReceiveConsumer.cpp:175-395`](ASFWDriver/Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp#L175-L395).

The record is emitted exactly once per established replay epoch, immediately
before the timing-loss callback.  It is anomaly-only; a normal stream produces
no such records.  `reason` is one of:

| Reason | Meaning | Discriminating fields |
| --- | --- | --- |
| `packet-status` | CIP/payload processing could not produce a usable packet. | `status`, `bytes`, `desc`, `rawTs`, `syt` |
| `invalid-rx-timestamp` | The descriptor's compact receive timestamp could not be correlated with the drain cycle timer. | `drain`, `rawTs`, `syt`, `validTs`, `invalidTs` |
| `receive-cycle-gap` | The strict one-received-packet-per-cycle invariant was broken. | `expectedCycle`, `observedCycle`, `desc` |
| `syt-cadence-rejected` | A valid-SYT observation produced a non-positive or out-of-range cadence delta. | `syt`, `observedCycle`, `rawTs` |
| `clock-anchor-rejected` | The host clock anchor was invalid when the RX side tried to publish it. | `frame`, `drain`, `rawTs`, `syt` |

From the repository root, after reproducing, retrieve it without Console:

```bash
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py \
  --endpoint http://127.0.0.1:8765/mcp \
  call asfw_log_query \
  '{"afterSequence":0,"categories":["DirectAudio"],"contains":"[RxReplayReset]","maxLevel":"debug","maxRecords":20}'
```

Interpret the first matching record before changing any SYT delay or recovery
constant.  In particular, `receive-cycle-gap` means ASFW did not observe a
packet in a cycle where the FireBug capture says the Duet normally sends one;
that points at IR DMA/descriptor/drain accounting, not raw cross-direction SYT
phase.  `syt-cadence-rejected` instead points at the device-timestamp/replay
model.  The other three paths identify their own layer directly.

## Confirmed TX audio-timeline underexposure

The Duet run at 20:30 on 2026-07-18 produced severe audible corruption, then
continued normally without a bus reset, CMP reconnect, or fatal stream
recovery.  Its retained driver records establish that this was not an isoch
descriptor shortage:

```text
[PayloadWriter] deficitMax=380 visitedDelta=6144 writtenDelta=2381
                withoutPktDelta=3760 outsidePktDelta=3
                underExpCallsDelta=12
[TxPrep] margin=355 ... maxLatUs=9472 late1500=4
```

The following measurements are representative across the contiguous anomaly
records:

| Measurement | Observed value | Consequence |
| --- | --- | --- |
| CoreAudio host frames offered per drain | 5,632–6,144 | The host continued to supply audio. |
| Frames written into AMDTP packets | 2,220–2,460 | Only about 40% reached packet images. |
| `framesWithoutPacket` | 3,378–3,823 | About 60% of offered frames were ahead of the packet timeline and were dropped. |
| `framesOutsidePacket` | 1–4 | This is not principally a stale/retired-slot race. |
| Maximum exposure deficit | 364–404 frames | The host was 7.6–8.4 ms ahead of exposed data packets at 48 kHz. |
| Maximum TX-preparation latency | 9.472 ms | A preparation scheduling slip consumed the usable data horizon. |
| TX descriptor margin | 355 packets | DMA still had descriptors to transmit; those descriptors did not guarantee valid host audio content. |

The relevant buffers must not be conflated:

- The host output ring is 1,536 frames (32 ms at 48 kHz).
- A normal maximum CoreAudio callback is 512 frames (10.7 ms).
- At the incident, the data exposure lead was 576 frames (12 ms).
- The hardware descriptor queue can remain full of safe NODATA packets even
  while the data-bearing audio timeline is too short.

### AppleFWAudio comparison: a half-ring content horizon

The attached IDA database's `AM824NuDCLWrite` decompilation is strong
behavioural evidence for the size and placement of a durable TX horizon.  This
is a static comparison only: field names are decompiler inferences, and ASFW
must not copy Apple implementation code.

Apple configures a default circular NuDCL ring of 100 buffer groups with eight
packets per group, i.e. 800 isochronous packet positions (local-only IDA
export `/Users/mrmidi/.idapro/AM824NuDCLWrite_methods.txt:1878-1906`).  At
stream start it derives the total packet count as their product (same export,
`6143-6147`).

Its normal-sync insert target is the live input/extract position plus measured
cycle displacement plus a fixed **400 packet-cycle** lead for the 48 kHz
family; 44.1-kHz-family rates use 450 (same export, `3758-3809`).  The
Mac-sync initialization path uses 400 or 476 packet cycles (same export,
`3818-3849`).
`PerformClientIOOutput()` writes only until its sample-insert packet cursor
reaches this target (same export, `3248-3253`).

For 48 kHz, 400 bus cycles is 50 ms and corresponds to approximately 2,400
audio frames (six frames/cycle on average).  The important property is the
relationship, not a magic number: Apple's content target is roughly **half of
an 800-cycle / 100-ms circular ring**, leaving the other half for in-flight
transport and wrap/reuse slack.

| Geometry | Incident ASFW Duet run | Attempted ASFW target (rejected below) | Apple 48 kHz writer pattern |
| --- | ---: | ---: | ---: |
| Data-bearing horizon | 576 frames / 12 ms | 400 cycles / 50 ms at 48 kHz | ~2,400 frames / 50 ms (400 cycles) |
| Packet backing ring | 408 cycles / 51 ms | 912 cycles / 114 ms | 800 cycles / 100 ms |
| Packet timeline | 512 slots | 1,024 slots | private NuDCL packet program |
| Layout | Data horizon is much shorter than the transport queue. | Content target remains below half the backing ring; OHCI's 48-packet ring is unchanged. | Content horizon is approximately half of the circular packet ring. |

This rules out treating 1,536 frames (32 ms) as a final target merely because
it is the current host output-ring size.  A future ASFW implementation may
write packet frames for future absolute sample times before those samples
arrive; the audio callback later fills those already-exposed packet images.
The packet timeline must therefore be sized independently from the host sample
ring, while preserving the audio-side ownership boundary.

`AmdtpPayloadWriter::WriteFloat32Interleaved()` only writes a host frame when
`SnapshotSlotForAudioFrame()` finds an already-exposed packet.  If not, it
increments `framesWithoutPacket` and permanently skips that frame
([`AmdtpPayloadWriter.cpp:90-126`](ASFWDriver/Audio/Wire/AMDTP/AmdtpPayloadWriter.cpp#L90-L126)).
When TX preparation later resumes, the RX-derived `TxAlign` path can reposition
the frame cursor to the current timeline ([`ASFWAudioDriverZts.cpp:360-389`](ASFWDriver/Audio/DriverKit/ASFWAudioDriverZts.cpp#L360-L389)).
That is the observed “self-heal”: future samples become writable again; the
discarded samples are never recovered, so a click/gap is unavoidable.

### Follow-up observation: corrupt audio can later become clean without a restart

In the same deployed Duet stream, a later interval at 20:36 showed complete
content starvation for consecutive drains: `writtenDelta=0` while
`withoutPktDelta=5,632–6,144`, with `outsidePktDelta=0`.  The user did not
stop the stream, rebuild, redeploy, reconnect CMP, or touch the device.  It
continued to emit badly corrupted audio for a period, then became audibly
clean by itself.

The subsequent 20:37–20:38 records still showed continuous TX SYT/ZTS
progress and descriptor margins of 277–354; current preparation latency was
30–265 us.  The retained `maxLatUs=9472` and `late1500=8` values were
historical high-water counters from the preceding incident.  There was no
logged bus reset, CMP reconnect, IT restart, or fatal recovery in that slice.

This is **not** evidence that the old stream has been repaired.  It proves
that DMA/clock liveness and audio-content correctness are independent in the
current implementation: once a future host range and a future exposed packet
range overlap again, the existing re-alignment path can make newly emitted
audio coherent.  Frames dropped during the bad interval remain lost.  The
next fault must record the packetizer cursor/alignment state as well as the
first deficit range, so that the transition back to overlap is attributable
rather than inferred.

### Duet validation: deep preparation is not a valid content horizon

The first Duet validation of the 400-cycle scheduler rejected the initial
implementation. It started only after restoring a **full 912-slot NODATA
prefill**: `ASFWAudioDevice::StartIO()` deliberately verifies
`committedEnd == numSlots` before IT arm, so a 678-slot lead-only prefill fails
fast at `ValidateTxPrefill`. That startup contract is transport-correct, but
the full prefill alone puts the first reusable packet slot roughly 114 ms after
IT start and is unsuitable as a live-instrument latency policy.

More importantly, the running stream developed periodic silence and then total
silence even while TX SYT and descriptor coverage remained healthy. MCP ring
evidence showed all of the following:

- `[TxPrepRange]` repeatedly reached its packet limit with `short=0` and
  `marginAfter=678`, yet was emitted because its frame target was short. This
  rules out an OHCI refill hole.
- No `[RxReplayReset]` record occurred, so the receive replay epoch was not
  reset by a detected RX discontinuity.
- `[TxAlign]` appeared repeatedly. A healthy stream emits it once at start;
  this line is reached only after `txReplayReader.TryRead()` fails, re-arms the
  packetizer cursor, and later receives a readable replay entry.

The mechanism is now confirmed. `PrepareTransmitSlots()` consumes one
`RxSequenceReplay` timing entry for every future TX slot it packetizes. If the
read fails, it intentionally emits a legal NODATA packet rather than block
the IT producer, then calls `ReArmFrameCursorAlignment()`. The next readable
entry aligns the packetizer to a newer projected frame, abandoning the prior
host-frame range. Repetition creates periodic holes; continuous failure creates
only NODATA packets, i.e. complete silence, while FireWire/CIP/SYT and DMA
continue normally.

The first implementation combined a 678-packet preparation lead with a replay
reader that starts only 256 entries behind a 512-entry RX history
(`RxSequenceReplayState::kCapacity` / `kReadDelay`). That violates the required
independence: a deep TX content reservation must not consume the finite
RX-derived SYT replay history as though it were future timing. The immediate
failure is `TryRead()`; whether a given miss is reader-ahead, overwritten slot,
or epoch mismatch still needs compact first-fault telemetry
(`readerNext`, `replayProducer`, `epoch`, packet index). It must be recorded
before selecting the final timing-cache/prediction policy.

**Do not treat the 912/678/400 implementation as a viable Duet configuration.**
The correct design separates three horizons:

1. Full transport prefill, solely to satisfy IT arm/ownership safety.
2. PCM slot reservation, deep enough for the CoreAudio burst horizon.
3. SYT replay/prediction, only as deep as valid RX timing is retained or can
   be deterministically predicted.

### Attempted scheduler change; remaining hardening

Increasing the DMA descriptor count alone is insufficient. The following was
implemented for the first Duet validation, but the preceding result means it
must be redesigned rather than extended:

1. TX preparation was made a data-horizon scheduler as well as a descriptor
   refill scheduler. Every CoreAudio `WriteEnd` publishes an audio-owned
   absolute `targetFrameEnd` and signals the existing preparation action
   through a coalescing latch. Preparation drains the latest target on its
   dedicated queue; the real-time callback never manipulates transport
   cursors.
2. The target was set to 400 FireWire cycles, converted at the active stream rate
   (2,400 frames / 50 ms at 48 kHz; 2,205 frames / 50 ms at 44.1 kHz).
   Packet backing is now 912 slots and the packet timeline is 1,024 slots.
   The 48-packet OHCI descriptor ring and its 144-packet refill-coverage
   budget are unchanged.
3. A future hardening step may preserve a pending absolute host-frame range whenever a packet slot is not
   yet exposed, then flush it once the timeline exposes that range.  It may
   only discard a frame after proving the CoreAudio ring has overwritten it;
   it must account for that as an xrun.
4. Descriptor/NODATA prefill remains separate from this guarantee. A full DMA
   queue is transport liveness, not audio-content liveness.
5. Continue to report `framesWithoutPacket`, first deficit range, timeline
   exposure end, TX preparation latency, pending-range age, and packetizer
   absolute cursor/alignment state as anomaly-only MCP-ring telemetry.

The first-deficit log now records the CoreAudio range, exposed end, deficit,
completion cursor, playback range, packetizer absolute cursor/alignment epoch,
and its last DATA packet range.  It also records total/DATA/NODATA preparation
and slot-acquisition-failure counters, plus the audio-frontier target,
requested/handled generations, and coalesced-wake state. These are lock-free,
best-effort diagnostic snapshots across the callback/preparation boundary;
they must never be used to control the stream. The attempted scheduler cannot
be the primary fix for the observed `W > E` loss until it stops consuming
replay timing beyond the valid horizon. The pending-range redesign remains
defense in depth for an exposure failure exceeding the eventual content horizon.

The MCP driver ring retains only a short message prefix (about 256 bytes).
Anomaly records are now deliberately split into independent compact records:
`[TxReplay]` reports the exact `TryRead()` result; `[TxPrepRange]` reports
packet coverage; `[TxPrepFrame]` reports frame exposure; and `[PayloadWriter]`
reports deltas, last state, and (when present) the first deficit. Each puts its
decision fields first and remains independently useful if the ring truncates a
record.

`[TxReplay] fail=` has the following precise meanings:

- `inactive`: the reader could not begin because RX history is not established.
- `epoch`: RX reset/discontinuity invalidated the reader's snapshot.
- `ahead`: TX consumed the newest RX entry and requested one RX has not yet
  published.
- `overwritten`: TX stopped long enough for the 512-entry RX history to wrap
  past its reader cursor.
- `slot-seq`, `slot-epoch`, or `slot-changed`: the bounded lock-free slot was
  unavailable or changed during its seqlock-style read.

This separates a genuine RX interruption from the current, more likely design
error: future TX packet preparation consuming a finite history of past RX SYT
observations.

### The actual TX failure: content reaches an unprepared region

The primary fault model is a cursor/geometry violation, not necessarily an
OHCI DMA read of random memory. The transport ring is deliberately prefilled
with legal NODATA packets, so OHCI can remain healthy while it transmits no
audio content. Four independent cursors currently lack one enforced contract:

```text
W = CoreAudio write end          host frames which exist
E = exposed frame end            host frames mapped into prepared TX packets
C = completion/committed end     packet slots safe for OHCI ownership/reuse
R = RX replay read/producer      observed timing history available for TX SYT
```

The invalid states are:

```text
W > E                 payload writer reaches a host frame with no packet slot
R.read >= R.producer  TX requests an RX timing observation that does not exist
```

Today both violations are treated as recoverable branches: the payload writer
counts `withoutPacket` and permanently skips the samples; `TryRead()` emits a
legal NODATA packet and re-arms `TxAlign`. When replay becomes readable again,
alignment jumps to a newer frame range. The transport survives, but the
unprepared interval is lost; repetition sounds like clicks/periodic silence and
continuous failure is complete silence.

The required contract is non-negotiable:

1. Never consume a CoreAudio frame until an exposed TX packet owns that frame.
   Retain it pending, or declare an explicit xrun only after its host-ring
   storage is proven overwritten.
2. Never consume RX replay beyond valid observed history. Future TX packet
   reservation requires a timing cache/predictor with an explicit validity
   horizon, not direct consumption of a finite historical ring.
3. `C` protects DMA ownership only; NODATA prefill is not evidence that audio
   content is prepared.
4. On recovery, reset/re-prime W/E/C/R under one epoch before transport runs;
   do not use `TxAlign` to silently abandon the old interval.

## 2026-07-19 zombie stream: transport lap loss confirmed as the driver

The 44-minute Duet run of 2026-07-19 (FireBug capture `!duet-fubar.txt` plus
the full MCP driver ring) finally attributed the "transport-alive,
audio-all-zero" state to a transport accounting defect underneath everything
in this document.  The wire showed both channels at one packet per cycle with
0 silent cycles and channel 0 carrying 72-byte DATA packets whose SYT tracked
channel 1 exactly — with every PCM sample zero.

Measured from the retained ring (17.3-minute window, zero dropped records):

| Quantity | Expected | Measured |
| --- | ---: | ---: |
| RX replay entries published | 8,000/s | 7,995/s (healthy) |
| TX completion cursor advance | 8,000/s | ~575/s |
| TX packets prepared | 8,000/s | ~575/s |
| `[TxAlign]` realignments | 1 per stream | every prepare batch (~28,000 total) |
| `[TxReplay] fail=overwritten` | none | ~13/s at `d=-547..-920`, `ep=4/4` |
| PayloadWriter frames written | ~all | ~0 (`outside` ≈ all visited) |

At stop, the context reported `6,228,647 pkts IRQs=810214` against ~21.4 M
wire packets: the IT interrupt path ran at its healthy ~1,333/s for roughly
the first ten minutes, then went permanently silent; the `Poll()` watchdog
(~68 ms cadence) carried the stream for the remaining half hour.

The causal chain, each link now confirmed in code:

1. The IT descriptor ring is circular and free-running; hardware never stops.
   `ComputeDeltaConsumed()` measured progress from the command pointer, which
   is only defined **modulo the 48-packet ring**.  Any refill gap longer than
   6 ms silently discards whole laps: the wire re-transmits the stale 48
   slots (a DBC seam per lap, invisible in SYT because 48 ≡ 0 mod 16 cycles)
   while `completionCursor` under-advances with no fault raised.  Under the
   watchdog cadence this lost ~10 laps per kick — 71% of the session's wire
   packets were stale re-sends.
2. The producer paces itself from `completionCursor`, so producer time
   dilated to ~7% of bus time.  The replay reader consumed ~575 entries/s
   against RX's 8,000/s and fell out of its 512-entry window every few tens
   of milliseconds (`fail=overwritten` with `ep` unchanged — no RX reset).
3. Every replay miss reset the reader **and re-armed frame-cursor
   alignment**; the next DATA read re-projected the cursor ahead of the host
   write frontier, so the payload writer classified essentially every host
   frame `outside` and the pre-zeroed DATA payloads shipped as silence.  The
   loop is self-sustaining: transport looks healthy, audio is permanently
   dead.

### Implemented fixes (2026-07-19)

1. **True-cycle completion accounting** (`IsochTxDmaRing`):
   `ComputeLostLapPackets()` recovers true progress from the OUTPUT_LAST
   completion timestamps (the context transmits exactly one packet per
   cycle), detects whole lost laps — including the pointer-unmoved
   exactly-one-lap stall — and reconciles `completionCursor`,
   `softwareFillAbsIdx_`, and the completion stamps forward.  New counters
   `lapLossEvents`/`lapLossPacketsTotal` plus an `IT LAP LOSS` ring record
   make every event attributable.  The stale packets already on the wire are
   acknowledged as a bounded glitch, not silently converted into permanent
   time dilation.
2. **Sustained interrupt-silence fault** (`IsochTransmitContext::Poll()`):
   the watchdog now logs its first engagement and, after 16 consecutive
   IRQ-silent kicks (~1 s), stops the context via the existing TX-fault path.
   Watchdog-carried streaming re-transmits stale laps between kicks and is
   never a valid steady state.
3. **Failure-class replay handling** (`PrepareTransmitSlots()`):
   `overwritten` re-anchors the reader via `Begin()` and retries in place
   (`[TxReplay] reclamped`, no alignment re-arm — skipped entries only shift
   NODATA placement, which IEC 61883-6 blocking permits); `ahead` holds the
   reader and ships one NODATA; only a genuine RX-domain transition (epoch
   change, establishment loss, seqlock miss) resets the reader and re-arms
   alignment.  This removes the realign churn that orphaned host frames.

Host-side regression coverage: `IsochTxLapLossMath.*`,
`IsochTxDmaRingTest.RefillReconcilesLostLapForward`,
`IsochTxDmaRingTest.RefillDetectsWholeLapStallWithUnmovedPointer`,
`RxDrivenTimingTests.ReaderReBeginReanchorsAfterHistoryOverwritten`.

### 2026-07-19 Saffire Pro 24 DSP run: dual-context freeze and honest death

The first DICE validation of the lap-loss work (Saffire Pro 24 DSP, 48 kHz,
DICE backend) both validated and corrected it.  The stream started cleanly
and reconciled five single-lap losses in its first 35 seconds.  At ~67 s —
shortly after the device issued `ExtStatus`/clock notifications — **both of
our OHCI isoch contexts froze for ~8 seconds** while the wire and async
traffic stayed alive (FireBug `!saffire-wire.txt`: channel 1 never missed a
cycle; no bus reset; no `cycleInconsistent` interrupt; host latencies were
double-digit microseconds up to the freeze).  The IR side later recovered on
its own; the IT side crawled 13 packets, then died via
`IT FATAL: uncommitted slot` at ~75 s.

The wire capture corrected the lap-loss interpretation: channel 0 carried
exactly 533,821 packets — the freeze transmitted **nothing**, while the
completion timestamps implied 64,032 packets of progress.  A stalled context
whose sparse crawl stamps late timestamps is indistinguishable, at the
timestamp level, from free-running stale re-transmission.  The 2026-07-19
follow-up therefore classifies by coverage: unaccounted progress within the
producer's committed lead is reconciled (the Duet case); progress beyond it
can only end at an uncommitted slot, so the refill now faults immediately as
`context-stalled` with untouched cursors, and both paths record
`ContextControl` plus the latched `IntEvent` bits (visible even when masked)
as first-fault evidence for the freeze trigger.

The fatal also demonstrated the missing upward propagation
(`TX-FAULT-PROP-001`): after `IT FATAL STOP` + `[TxProducerFatal]`, CoreAudio
kept running IO against the dead stream indefinitely (the `[PayloadWriter]`
deficit passed 1.5 M frames), the DICE backend read the device's next
notification and confirmed it "healthy", no `[RxReplayReset]` ever fired
(total RX silence produces no next packet to detect a gap on), and the IRM
bandwidth/channels were never released.

### Still open after these fixes

- `TX-IRQ-001` — **why the IT interrupt path died ~10 minutes in.** The ring
  had already wrapped past the transition.  The new first-engagement log and
  lap-loss counters will timestamp the next occurrence; correlate against
  `IsoXmitIntMask` writes (the stop/fault paths mask IT interrupts).
- The FW-64 items (`AUDIO-RECOVERY-ROUTING-001` callback overwrite,
  `AVC-RECOVERY-002` unsafe background restart) are unchanged by this work
  and remain confirmed defects; they were not required to produce this
  incident.
- The pending-frame model of `AVC-TX-EXPOSURE-001` (retain host frames until
  slots exist) remains the durable repair for exposure lag; with the
  transport clock no longer dilating it is defense in depth rather than the
  first-order fault.

## 2026-07-19 architectural root cause: backend boundary leakage (dice ≠ avc ≠ bebob)

The Saffire Pro 24 DSP run with the lap-loss detector installed forced a
reassessment of the whole 2026-07-19 line of work. Two facts collapsed it:

1. **The lap-loss detector was killing a healthy Saffire stream.** Every
   `IT LAP LOSS` event on that run reported a mod-48 `delta` **below 48**
   (`0, 3, 5, 8, 9, 11, 18, 19, 23, 26, 27, 31, 40`), so the hardware never
   advanced a full ring between refills — mod-48 accounting was *correct the
   entire time*. The timestamp-based detector was double-counting a stale
   completion baseline under coalesced interrupts and inventing laps that
   were never on the wire; the `delta=0 → "2832 packets"` fatal is that bug
   run away (59 phantom laps). FireBug confirms the stream was continuous —
   2,241,634 packets with real PCM — until *our own* fatal fired. The
   detector is the regression.
2. **DICE was stable before any of this.** PR #41 (merge `1bbb70e`,
   2026-07-14) had no lap-loss logic at all — pure mod-48
   `ComputeDeltaConsumed` — and DICE streamed cleanly. The zombie only
   appeared after the 2026-07-18 receive-seam / FW-64 refactors.

So the lap-loss reconciliation treats a *symptom* of `TX-IRQ-001` (the Duet's
dead interrupt path, where 68 ms watchdog gaps genuinely exceed the 6 ms
ring) and actively breaks the healthy-coalescing case. It is also a
**boundary violation**: content/timing reconciliation shoved into the
payload-opaque transport (`IsochTxDmaRing`). It should be reverted.

### The real defect is cross-backend leakage, and the reference stacks prove the boundary

`references/linux-sound-firewire-stack` enforces `dice ≠ avc ≠ bebob` with
three rules ASFW currently breaks:

1. **Each family is its own driver; recovery is per-family policy.** `dice/`,
   `bebob/`, `oxfw/`, `fireworks/`, `motu/`, `tascam/`, `digi00x/` each own a
   `*_stream.c` with their own recovery entry point —
   `snd_dice_stream_update_duplex()` (dice-stream.c:587),
   `snd_oxfw_stream_update_duplex()` (oxfw-stream.c:472),
   `snd_efw_stream_update_duplex()` (fireworks). DICE's is pure DICE
   semantics (force-stop on bus reset because the firmware stalls for
   hundreds of ms, then let the app restart) — meaningless for AV/C or BeBoB.
   There is no shared `recover_streaming`.
2. **The shared core is mechanism and family-blind.** `amdtp-stream.c`,
   `cmp.c`, `fcp.c`, `iso-resources.c` contain no backend branching — only
   device-*quirk* flags (OXFW970 packet-skip, Dice high-rate). The engine
   moves packets and does not know who owns the stream.
3. **The seam is one content callback; errors flow up as state.** The backend
   injects `process_ctx_payloads` + an opaque `protocol` pointer
   (amdtp-stream.h:205-207). On trouble the engine sets its own state and the
   family **polls** `amdtp_streaming_error(s)` (amdtp-stream.h:256) and
   decides. Reporting is upward; policy stays in the family. The mechanism
   never calls a backend's recovery.

The Focusrite DICE kext (IDA, `Saffire.i64`) corroborates rule 1 from the
Apple side: its `NotificationWriteCallback` (0x9620) decodes the DICE
notification register and calls `RequestStreamingRestart` — DICE recovery is
driven by **DICE notifications**, guarded against re-entrancy. Its
`adjustOutputPhase` (0xc9c2) is a bounded phase servo in the full 8-second
tick domain (`% 0xBB80000`) that tolerates a standing RX/TX offset and only
re-anchors past a threshold — behavioural proof that a constant SYT offset is
normal (the "SYT OOS" capture was healthy), never a fault.

### Where ASFW leaks across the boundary

- **A single shared `IsochService::timingLossCallback_`** that both
  `DiceAudioBackend` and `AVCAudioBackend` write (last-writer-wins). AV/C
  overwrites DICE's, so a DICE discontinuity is routed to AV/C's non-matching
  GUID and dropped. Linux never shares a recovery callback.
- **`RecoverStreaming` as cross-backend machinery** driven by an RX-replay-
  reset signal common to both backends, instead of each backend's own device
  semantics.
- **The lap-loss detector** — content/timing policy inside the transport
  mechanism, the inverse leak.

### Corrective boundary (Linux- and Focusrite-validated)

1. Transport (`Isoch/`, `IsochService`, `IsochTxDmaRing`) is family-agnostic:
   move packets, publish own fault state, stop. No backend knowledge, no
   `RecoverStreaming` dispatch, no replay/timing reconciliation. → **revert
   the lap-loss detector**; keep only the transport-state pieces (watchdog-
   silence fatal + `ctrl`/`intEvent` snapshot).
2. **Delete the shared `timingLossCallback_` slot.** Transport exposes a
   state the backends poll (the existing `statusWord`/`fatalGeneration` is the
   `amdtp_streaming_error` analog). No cross-backend callback.
3. **Each backend owns its own recovery.** DICE recovery keyed off DICE
   notifications (both references agree); AV/C and BeBoB separate. No
   backend's recovery reachable from another's signal.

Sequence: (a) revert lap-loss, (b) sever the shared slot and make transport
report-only, (c) re-home DICE recovery onto DICE notifications.
`TX-IRQ-001` (why Duet interrupts die) remains an independent open root cause.

## Minimal anomaly-only telemetry for the next hardware run

Keep the MCP ring as the primary evidence path; no Console logging is needed
for this diagnosis.  Emit only on state transition or anomaly, with recovery
epoch and direction in every record:

```text
[RxClock] E=.. validSyt/run, packetCycle, fullPresentationTicks, dbc,
          dataBlocks, sytDeltaTicks, ratePpm, discontinuityReason
[TxClock] E=.. outputCycle, sourcePresentationTicks, txDelayTicks,
          encodedSyt, dbc, dataBlocks, queueLead
[PayloadWriter] firstCoreAudioRange, firstExposedEnd, firstDeficitFrames,
                completionCursor, playbackRingRange, miss-counter deltas,
                firstPacketizer={next, aligned, epoch, lastPacket, lastRange},
                lastPacketizer={next, aligned, epoch, lastPacket, lastRange},
                prepared={all, data, noData, acquireFail}
[DuplexPhase] E=.. rxPresentationTicks, txPresentationTicks,
              rxDelay, txDelay, normalizedPhaseTicks, phasePpm
[Recovery] E=.. producerQuiesced, producerReset, prefillCommitted/slots,
             transportArmed, cmpConnected, firstValidRx
```

Thresholds should be direction-local: one malformed/no-info packet is not a
clock failure; a DBC/cycle discontinuity, invalid valid-SYT cadence, or a
timeout without a fresh valid RX anchor is.  The implemented first-fault record
identifies the present reset path; add the remaining normalized RX/TX phase and
recovery-epoch records only after that result identifies the layer at fault.

## Duet discovery: FCP clock prepublish must not hide a usable device

### Evidence

During the 2026-07-19 no-nub incident, discovery read Duet's live rate as
44.1 kHz, then started the optional fixed-48-kHz prepublish sequence. The
discovery watchdog completed it after 1,200 ms, while FCP's first response
deadline is 2,000 ms. The failure path then deferred the nub entirely, so no
AudioDriverKit device could reach Audio MIDI Setup.

The FireBug trace shows that Duet uses the standard, expected choreography:
an 8-byte `STATUS` or `CONTROL` Unit Plug Signal Format command (opcodes
`0x19` input then `0x18` output) is answered by an 8-byte FCP block write to
the initiator's `0xfffff0000d00` response register. This matches the command
form used by ASFW and Linux OXFW's input-before-output ordering
(`references/linux-upstream/sound/firewire/oxfw/oxfw-stream.c:41-54`). The
device does not reject the standard format sequence.

### Root cause and implemented fix

ASFW drains AR Request before AR Response. The FCP payload is a block write in
AR Request; the write acknowledgement for our preceding command is in AR
Response. `FCPTransport::OnFCPResponse()` previously rejected a valid FCP
payload until the latter callback had set `successfulWriteAttempt`. The exact
Duet wire order therefore became:

```text
Duet FCP response arrives in AR Request  → dropped as "before write completion"
our write acknowledgement arrives in AR Response → start a 2 s FCP timer
no second FCP payload exists             → timeout, no verified 48 kHz, no nub
```

The FCP response itself proves that the target received and processed the
command. The transport now accepts a validated response for its active
node/generation/write-attempt even if the local write-completion callback has
not run yet. A later write callback is safely stale after the FCP command has
completed. The Duet remains fixed to 48 kHz: no fallback rate is advertised or
published.
