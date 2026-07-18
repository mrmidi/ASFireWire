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
| `AVC-PROPERTY-001` | **Fixed; requires hardware confirmation** | `AudioNubPublisher` sets stream mode on the nub, but `ASFWAudioDevice::PopulateNubProperties()` did not publish `ASFWStreamMode`; the driver parser consequently defaulted to non-blocking.  The model default is also non-blocking ([`ASFWAudioDevice.hpp:52-60`](ASFWDriver/Audio/Model/ASFWAudioDevice.hpp#L52-L60)); parsing only changes it if the property exists ([`AudioDriverConfig.cpp:67-84`](ASFWDriver/Audio/DriverKit/Config/AudioDriverConfig.cpp#L67-L84)).  The matching dictionary now publishes the selected mode. | On the next Duet/BeBoB start, confirm the AudioDriver reports `Stream mode from nub: blocking`; the property path is shared by all published audio nubs. |
| `AVC-TX-EXPOSURE-001` | **Confirmed audio-path defect; root scheduling cause open** | The payload writer skips CoreAudio frames which arrive beyond `Timeline().ExposedFrameEnd()` (`framesWithoutPacket`); `TxAlign` later repositions the producer cursor to close the deficit.  This can produce audible corruption while DMA/CMP and the TX descriptor lead remain healthy, then self-heal without a stream restart ([`AmdtpPayloadWriter.cpp:90-126`](ASFWDriver/Audio/Wire/AMDTP/AmdtpPayloadWriter.cpp#L90-L126), [`ASFWAudioDriverZts.cpp:360-389`](ASFWDriver/Audio/DriverKit/ASFWAudioDriverZts.cpp#L360-L389)). | Retain pending host frames until slots exist and maintain a data-bearing packet horizon beyond the maximum IO callback plus measured scheduling jitter; never discard a running stream's host frames because their packet is not yet exposed. |
| `AVC-SYNC-001` | **Not proven** | Treating raw RX/TX SYT inequality as a fault would be wrong for the captured duplex stream. | Add the normalized, per-direction trace below before adjusting delays or cadence. |
| `AVC-HEALTH-001` | **Instrumented; root cause open** | The original RX timing loss occurred without a bus reset in the observed driver ring; successful restart was then masked by `AVC-RECOVERY-001`. | Reproduce with the first-fault record below; it distinguishes a local RX validation failure from a real no-RX/CMP outage. |

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
