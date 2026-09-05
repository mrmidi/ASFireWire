# ASFW Round-Trip Latency and TX Content Ownership

Status: design and evidence record, 2026-08-29
Source tree inspected: `3ab346e8` plus the Gate A2 working tree described below.
Implementation status: Apple Duet HAL-accounting parity is implemented and
host-validated; hardware acceptance is still required.

This document records the latency investigation that followed Audio Engine V3.
It separates measured facts, reverse-engineering evidence, current ASFW
contracts, design conclusions, and remaining hardware questions. It is not a
replacement for `AUDIOENGINEV3_DRAFT.md`; it records how V3 separated deep
transport preparation from the short payload-finality boundary.

## Executive conclusion

The V3 hardware timeline, absolute sample coordinates, 8,192-frame HAL/cache
geometry, and backend-specific SYT conversion remain the right foundation.

The main output-latency defect was a different contract:

```text
ASFW before Gate A2

packet planned for future transport
        =
complete PCM packet published
        =
payload immutable until OHCI completion
```

This couples three quantities that must be independent:

1. cadence/presentation planning depth;
2. descriptor or transport runway;
3. the last instant at which HAL PCM can still affect a presentation slot.

AudioDriverKit itself separates client-ring position from hardware position,
and expresses the safe distance between them through the input/output safety
offsets. AppleFWAudio's private clip and DMA accessors are a kext-era
implementation of the same basic HAL distinction, not independent proof of a
special late-binding architecture.

The stronger AppleFWAudio evidence is in its data path, but it must be compared
at the correct layer. At the audio/content layer, AppleFWAudio creates cyclic
send commands backed by packet buffers containing valid silence, records client
output ranges by absolute sample frame, maps those frames into the future packet
buffers, and overwrites only their PCM words. At the transport layer,
AppleFWOHCI separately compiles those send commands into physical OHCI DMA
descriptor blocks.

ASFW already has the corresponding layer split: Audio/Wire owns the opaque
packet bytes in the shared producer payload slab, while `IsochTxDmaRing` maps
those slots into a separate 48-packet OHCI descriptor ring. The current latency
coupling is not caused by using OHCI descriptors directly. It is caused by the
ASFW seam treating producer-slot commit as both transport availability and
permanent payload finality.

Gate A2 now models the stable planned producer slot separately from its
writable payload, descriptor mapping, physical finality boundary, and
completion. A packet is armed with complete silence in image 0. Image 1 may be
published after descriptor binding. Because A1 split the invariant eight-byte
prefix from the mutable tail, transport can select image 1 by changing one
aligned `OUTPUT_LAST.dataAddress` store while leaving every other descriptor
field unchanged.

`mappedEnd` now means descriptor ownership only. `finalizedEnd` is the content
boundary and advances from the absolute command-pointer position by one
six-packet completion interval plus a two-packet repoint guard. The 120-slot
prepared runway is unchanged and no longer appears in HAL output safety.

No finality or HAL timing value is justified by the historical 40–42-cycle
DriverKit stall observation. Preparation stalls are absorbed by the independent
silence-armed runway. Current-path telemetry remains the acceptance source for
preparation and ownership margins, but it is not a reason to move finality back
to the mapping frontier.

## Scope and terminology

In this document:

- **fixed input/output cost** is the frame count Logic reports in addition to
  the selected client I/O buffer;
- **reported latency** is the driver's declared acquisition/delivery or
  handoff/presentation delay;
- **safety offset** is the hardware-relative distance at which client I/O is
  still safe;
- **planning lead** is how far the engine has assigned cadence, DBC, SYT,
  disposition, and absolute frames;
- **armed lead** is how far transport has a valid silence-initialized DATA or
  cadence NO-DATA packet;
- **content lead** is the distance between accepted client PCM and physical
  presentation;
- **freeze** is the transport-owned transition after which PCM words are no
  longer writable;
- **completion** is OHCI retirement and resource reuse, not presentation time.

The latency work does not restore packetizers, `WriteEnd`, staging, or RX replay
as clocks. The `HardwareSampleTimeline` remains the sole owner of absolute HAL
sample time within an epoch.

## AudioDriverKit already separates client and hardware position

The DriverKit 27.0 AudioDriverKit headers expose two distinct coordinate views:

| meaning | AudioDriverKit surface |
|---|---|
| client sample position in the shared ring | `GetCurrentClientSampleTime`, `GetCurrentClientIOTime`, and the I/O handler's `in_sample_time` |
| hardware sample/host-time anchor | `UpdateCurrentZeroTimestamp` and `GetCurrentZeroTimestamp` |
| safe client distance from hardware | `SetInputSafetyOffset` and `SetOutputSafetyOffset` |

`IOUserAudioIOOperationWriteEnd` is delivered after the host has written a
range into the output stream buffer. Its `in_sample_time` identifies where that
range occurs in the device timeline. `UpdateCurrentZeroTimestamp` supplies the
hardware-derived sample/host pair that drives the host's timing. The output
safety offset tells the host how far ahead of current hardware position output
I/O is safe.

Therefore V3 must not invent an independent client/content timeline:

```text
hardware presentation observation
        -> HardwareSampleTimeline
        -> UpdateCurrentZeroTimestamp
        -> ADK/HAL schedules client I/O in that sample timeline
        -> WriteEnd(sampleTime, frames) publishes that absolute range
        -> ASFW binds those bytes to the matching physical TX plans
```

AudioDriverKit does **not** expose or own the FireWire/OHCI details below that
boundary. There is no public ADK equivalent of an OHCI command pointer, a
payload-fetch frontier, or a physical packet-slot erase head. ASFW must still
own the distinction between a writable planned payload, a hardware-frozen
payload, and a completed/reusable slot.

The local SDK evidence is in:

- `AudioDriverKit.framework/Headers/AudioDriverKitTypes.h` for `WriteEnd` and
  `IOOperationHandler::in_sample_time`;
- `IOUserAudioClockDevice.iig` for client sample time and zero timestamps;
- `IOUserAudioDevice.iig` for per-direction client I/O time and safety offsets.

## Exact original-driver reference values

### Apogee Duet

The first measurements were reconstructed from Logic's displayed millisecond
values. Logic rounds those values, so they are useful but not exact at every
rate. The installed Apogee override plist supplies the exact split used by
AppleFWAudio:

| rate | input latency | input safety | input fixed | output latency | output safety | output fixed |
|---|---:|---:|---:|---:|---:|---:|
| 44.1 kHz | 46 | 46 | **92** | 55 | 46 | **101** |
| 48 kHz | 40 | 50 | **90** | 67 | 50 | **117** |
| 88.2 kHz | 108 | 92 | **200** | 57 | 92 | **149** |
| 96 kHz | 119 | 100 | **219** | 59 | 100 | **159** |

Reference:
`/Users/mrmidi/Desktop/new_kext/duet/DuetFWOverideDriver.kext/Contents/Info.plist`.

Consequences for the existing draft:

- the 48-kHz output target remains exactly **117 frames**;
- the exact 48-kHz input value is **90**, not the 88-frame estimate from
  rounded Logic output;
- the exact 44.1-kHz input value is 92, not 91;
- the exact 96-kHz values are output 159 and input 219, not 160 and 218.

The values are a reference-driver contract for the same hardware, not numbers
that ASFW may copy without implementing and measuring equivalent semantics.

### Historical ASFW comparison at 48 kHz

The V3 figures recorded from ASFW at `7ca8d6e3` were:

| direction | Apple fixed | ASFW fixed | excess |
|---|---:|---:|---:|
| output | 117 | 896 | 779 |
| input | 90 | 256 | 166 |

The output components explain nearly all of the excess:

```text
Apple: latency 67 + safety 50 = 117
ASFW:  latency 128 + safety 768 = 896

safety excess  = 768 - 50 = 718 frames
latency excess = 128 - 67 =  61 frames
```

The large ASFW safety value was not merely an incorrect property value. Under
the then-current immutable-packet contract, content really was committed that
far ahead. Gate A2 removes that contract; the current working tree reports the
same 117/90 fixed split as Apple.

## Evidence from AppleFWAudio

IDA database:
`/Users/mrmidi/Desktop/FWA_KEXT_CLEAN/AppleFWAudio-update.i64`.

### Private cursors mirror the HAL split

The original driver exposes distinct engine accessors including:

- `GetCurrentInputClipSampleFrame`;
- `GetCurrentOutputClipSampleFrame`;
- `GetCurrentInputDMASampleFrame`;
- `GetCurrentOutputDMASampleFrame`;
- `GetCurrentEraseHead`.

The clip-versus-hardware distinction is not unique evidence of AppleFWAudio's
internal buffering policy: modern AudioDriverKit exposes the equivalent client
ring and hardware-timestamp views directly. The private DMA and erase-head
accessors still show that FireWire command progress and physical reuse remain
driver-owned state below the HAL boundary. They do not, by themselves, prove
when future payload bytes are written.

### Latency properties are device policy

`AppleFWAudioDevice::GetLatencyInformation` at `0xe89e` reads a device-specific
latency dictionary selected by vendor, model, and sample rate. It obtains four
independent values:

- input latency;
- output latency;
- input safety offset;
- output safety offset.

`AppleFWAudioIsocEngine::SetUpSampleBuffer` at `0x1ac4` configures sample-buffer
geometry separately and then applies the latency/safety policy. Buffer capacity
is therefore not itself the reported safety or latency.

### Audio-layer packet storage and late client fill

The AppleFWAudio personality contains:

```text
NumBufferGroups     = 100
NumPacketsPerGroup  = 8
UsingNuDCLs         = true
```

This describes capacity for 800 send-command packet positions at the
AppleFWAudio/IOFireWireFamily seam. It does **not** prove an 800-packet content
lead, a 100-ms safety offset, an exact callback interval, or an 800-entry
physical OHCI descriptor ring.

The second IDA pass separated two paths that must not be conflated.

The cyclic-program maintenance path is:

- `AM824NuDCLWrite::NuDCLCallback` at `0x4aa50`;
- `AM824NuDCLWrite::TriggerOutput` at `0x4a050`;
- `AM824NuDCLWrite::Start` at `0x4ba2e`.

`Start` obtains an `IOFWLocalIsochPort` from the FireWire bus and builds the
cyclic NuDCL program. `TriggerOutput` obtains two virtual ranges for each send
command: a CIP-header range and a packet-data range. For a planned DATA packet
it copies a prebuilt AM824 silence/label template into the packet-data range.
For a NO-DATA packet it changes the data transfer length to zero. It collects
commands whose ranges/length changed and calls
`IOFWLocalIsochPort::notify(kFWNuDCLModifyNotification, ...)` once for the
batch. Selector `3` is defined as `kFWNuDCLModifyNotification` in the local
IOFireWireFamily headers.

The actual client-content path is:

- `AM824NuDCLWrite::PostProcessOutputSamples` at `0x47ff8`;
- `AM824NuDCLWrite::PerformClientIOOutput` at `0x48280`;
- `AM824NuDCLWrite::GetPacketIndexFromSampleIndex` at `0x496ce`;
- `AM824NuDCLWrite::EncodeRawAudioData` at `0x45f5a`.

`PostProcessOutputSamples` records `{firstSampleFrame, numSampleFrames}` in a
bounded 16-entry client-work queue. `PerformClientIOOutput` consumes those
ranges, maps the absolute client sample frame to a packet index and frame offset
inside the cyclic send-command storage, and encodes the client sample words
directly into the packet-data virtual range. `EncodeRawAudioData` performs the
actual output word store; absent channels produce the valid silent AM824 word.

`GetPacketIndexFromSampleIndex` derives a current DMA-cycle index from the
hardware timing state, starts its circular search eight packet positions behind
that index, and scans the packet-to-sample ledger for the requested sample. The
eight-packet search origin is not an output-safety constant or a proven update
margin. The function contains no independent PCM-freeze frontier; the HAL
safety contract is what is expected to keep client sample ranges away from
already-consumed hardware positions.

After performing client output work, `AM824NuDCLWrite::CheckForWork` calls
`IOFWLocalIsochPort::synchronizeWithIO`. That call is distinct from the
modify-notification path. In the recovered AppleFWOHCI implementation it gates
against the program's callback event source; it does not recompile every send
descriptor merely because PCM words changed.

The corrected audio-layer conclusion is:

```text
deep cyclic send-command storage and stable packet geometry
        +
valid silence already present in planned DATA payloads
        +
HAL supplies an absolute future client sample range
        +
client path overwrites only the matching packet-buffer PCM words
```

This establishes late content fill more directly than the private cursor names
or the command-array notification. It does not yet compare physical descriptor
ownership. That comparison follows at the matching transport layer.

### Physical descriptor translation: AppleFWOHCI versus ASFW

Additional IDA database:
`/Users/mrmidi/Desktop/AppleFWOHCI copy/AppleFWOHCI.i64`.

The Apple transport path is:

```text
AppleFWAudio AM824 packet buffers and IOFWSendDCL virtual ranges
        -> AppleFWOHCI_SendDCL::compile
        -> virtual-to-physical range translation
        -> one or more physical DMADescriptor records
        -> AppleFWOHCI DCL program/context
        -> OHCI
```

`AppleFWOHCI_SendDCL::compile` translates the send command's virtual ranges,
allocates a physical descriptor block, writes the immediate isoch header, and
writes descriptor entries for the payload segments. Its `link` method connects
that block to the next compiled command.

`AppleFWOHCI_DCLProgram::notify` handles
`kFWNuDCLModifyNotification` by recompiling and relinking each changed send
command. If the required physical descriptor-block size changes, the lower
layer allocates or resizes the block and retires the old one. This is the
physical consequence of the DATA/NO-DATA range-length change in
`TriggerOutput`; it is not the normal PCM-fill operation.

The matching ASFW transport path is:

```text
Audio/Wire packet bytes in producer payload slab + IsochTxPacketMeta
        -> commitGeneration
        -> IsochTxDmaRing::Prime / Refill
        -> four physical 16-byte OHCI blocks per hardware packet
           [OUTPUT_MORE_IMMEDIATE x2, OUTPUT_MORE, OUTPUT_LAST]
        -> OHCI
```

ASFW's layers and the closest Apple analogues are:

| concern | Apple stack | ASFW |
|---|---|---|
| planned opaque packet bytes | `IOFWSendDCL` virtual ranges and their backing packet buffers | shared producer payload-slab slot |
| packet metadata / availability | NuDCL program and changed-command notification | `IsochTxPacketMeta` and `commitGeneration` |
| transport compilation | `AppleFWOHCI_SendDCL::compile/link/update` | `IsochTxDmaRing::Prime/Refill` |
| physical transmit program | variable-size `DMADescriptor` block per compiled command | fixed four-block OHCI program per hardware packet |
| hardware progress / reuse | AppleFWOHCI DCL program/context state | command-pointer decode, completion status, and `completionCursor` |

ASFW currently has 168 audio-produced payload slots but only 48 physical OHCI
packet programs. On refill, `IsochTxDmaRing` resolves the selected producer
slot into up to two DMA payload fragments, publishes the payload to the device,
executes a publication barrier, and then publishes the modified physical
descriptor blocks. It later remaps that 48-entry physical ring to subsequent
absolute producer slots.

ASFW source anchors for this comparison are:

- `ASFWDriver/Audio/DriverKit/ASFWAudioDriverPrivate.hpp` for producer-slot
  acquisition, metadata publication, `commitGeneration`, and `payloadSeal`;
- `ASFWDriver/Isoch/Core/IsochTxQueue.hpp` for the neutral shared queue ABI;
- `ASFWDriver/Isoch/Transmit/IsochTxDmaRing.cpp` for payload-fragment mapping,
  four-block physical descriptor construction, DMA publication ordering,
  command-pointer progress, completion, and remapping.

Therefore no direct numerical equivalence is valid between Apple's 800 NuDCL
positions, ASFW's 168 producer slots, and ASFW's 48 physical descriptor packet
programs. The relevant behavioral comparison is narrower:

```text
Apple: compiled physical descriptor already addresses packet buffer;
       client path later changes only PCM words in that buffer.

ASFW today: physical descriptor addresses producer payload slot;
            producer commit seal forbids any later PCM-word change.
```

The ASFW payload seal is an explicit software ownership rule, not a necessity
demonstrated by the physical descriptor layout. Replacing that rule still
requires ASFW-specific proof of payload DMA visibility and a safe distance from
the real OHCI command pointer/fetch frontier. AppleFWOHCI establishes the
architecture, not the numerical guard for this DriverKit implementation.

## Evidence from the Saffire DICE driver

IDA database: `/Users/mrmidi/Desktop/Saffire.i64`.

### Content path cross-check

`SaffireAudioEngine::clipOutputSamples` at `0x16580` converts client float PCM
to big-endian signed 24-in-32 samples and writes it directly into a preallocated
FireWire output buffer group. Placement is derived from the absolute
`firstSampleFrame`, the engine ring size, and a stream-specific sample offset,
then reduced to a buffer-group index and an intra-group frame offset.

`Saffire::FillFirewireBuffers` at `0xe778` manages the corresponding future
wire-packet storage: it clears packet payload regions to a valid baseline,
computes/stamps CIP and SYT state, and links or recycles the cyclic packet
buffers. The exact name of the stream-offset field used by
`clipOutputSamples` is not yet resolved and is deliberately not inferred from
its structure offset.

This is a second, independent content-layer example of client PCM being placed
into preallocated future FireWire packet storage. Its ASFW analogue is the
producer payload slab, not `IsochTxDmaRing`'s OHCI descriptor slab. The Saffire
binary does not by itself establish ASFW's physical write/fetch guard.

### Reported latency and safety policy

`Saffire::UpdateIsochBufferParams` at `0xf506` implements the same mode-1
safety ladder currently represented by ASFW:

```text
48 kHz:       output 6 packets,  input 16 packets
96 kHz:       output 8 packets,  input 18 packets
176.4/192 k: output 10 packets, input 20 packets
```

`SaffireAudioEngine::updateSampleLatencies` at `0x284a` supplies the separate
reported-latency ladder:

```text
1x rate: 29 frames
2x rate: 59 frames
4x rate: 119 frames
```

At 48 kHz, where ASFW's DICE profile uses eight frames per DATA packet, the
original driver's base components before any device-specific or user-added
offsets are therefore:

```text
output: 29 + 6 * 8  =  77 frames
input:  29 + 16 * 8 = 157 frames
```

This validates the existing DICE base profile constants. A generic V3 safety
calculation must not replace them merely because the cadence planner has a much
longer horizon.

The DICE evidence does not prove that every family or device uses the Saffire
numbers. Latency and safety remain device/backend policy backed by measurement
or a reference implementation.

## Linux cross-validation

The local Linux FireWire references support the same separation without being
used as source code:

- `sound/firewire/amdtp-stream.c` sizes a packet queue independently from the
  playback-delay calculation and processes outgoing payloads as completed queue
  entries are recycled;
- `drivers/firewire/ohci.c::queue_iso_transmit` builds the OHCI descriptors,
  points them at payload storage, synchronizes DMA-visible memory, and then
  publishes the descriptor chain.

The transferable behavioral lesson is that queue capacity, PCM delay, and
reported runtime delay are distinct. Linux does not by itself prove that an
already-published ASFW descriptor may be rewritten safely.

References:

- `references/linux-upstream/sound/firewire/amdtp-stream.c`;
- `references/linux-upstream/drivers/firewire/ohci.c`.

## Current ASFW contract after Gate A2

### Planning depth is independent from safety

`AudioTimingGeometry` currently defines:

```text
hardware/ownership slots = 48
dispatch slack slots     = 72
prepared target          = 120
shared packet slots      = 168
```

These are preparation/runway values, not latency values. Packets are armed with
a complete valid image throughout this horizon, so a delayed producer causes a
content substitution, not a descriptor-chain hole.

The payload-finality lead is independently defined as eight cycle slots: six
until the next transport completion plus a two-packet guard ahead of the live
OHCI command pointer. At 48 kHz this is 48 nominal frames; the Duet profile's
50-frame output-safety value wins. Backend transfer delay is reported as
latency and is not counted again as safety. Safety is not rounded to the
32-frame ring-alignment grid.

### Publication and finality are separate

`IAmdtpTxSlotProvider` exposes the planned-slot publication path plus a late
payload path:

```cpp
AcquireWritableSlot(packetIndex, slot)
PublishSlot(packet)
FinalizedEnd()
AcquireLatePayloadSlot(packetIndex, slot)
PublishLatePayload(packetIndex)
```

`PublishSlot` commits packet geometry and a valid silence image, not final PCM.
The audio service writes only image 1. Transport alone selects that image,
publishes it to DMA, and either binds it initially or repoints the already-bound
mutable tail. At completion it checks the seal of the image it selected. A race
resolves to one complete image; the producer never mutates the image currently
addressed by hardware.

### Producer slots and physical descriptors are already separate

ASFW does not have one monolithic packet/descriptor ring:

- Audio/Wire publishes into 168 slots in the shared payload slab;
- `IsochTxDmaRing` owns 48 physical packet programs;
- each physical packet program contains four 16-byte OHCI blocks;
- `Prime`/`Refill` maps successive absolute producer slots into those physical
  programs;
- refill publishes payload DMA visibility, issues a barrier, then publishes the
  physical descriptor changes;
- the hardware command pointer and completion status drive physical progress.

`commitGeneration` publishes planned geometry. `pcmGeneration` publishes image
1. `payloadSeal` is transport-owned and covers the complete image selected for
transmission. The content/transport layer boundary is unchanged; the mechanism
is expressed entirely in ASFW producer slots and four-block OHCI programs, not
as a numerical analogy to NuDCL.

### Duet timing is exact profile policy

`OxfwProfileBuilder` now scopes the original AppleFWAudio table to the Apogee
Duet personality:

```text
rate     input latency  output latency  input safety  output safety
44.1 kHz      46              55             46             46
48.0 kHz      40              67             50             50
```

Graph construction reports the profile values directly. The removed generic
input-jitter floor no longer overwrites 50 with 128. These are HAL accounting
values; matching them does not by itself establish physical loopback latency.

## Contracts that remain unchanged

The latency fix must preserve these V3 decisions:

1. Hardware presentation time is the sole timing authority.
2. `WriteEnd(S,N)` publishes completed PCM bytes only. It does not advance
   clocks or transport cursors.
3. `PcmPublicationCache` is retention, not scheduling.
4. Packetizers receive explicit absolute frames and presentation plans.
5. Backend code owns cadence, DBC, SYT, NO-DATA representation, and
   transfer-to-presentation conversion.
6. RX replay may inform backend wire timing but never owns HAL frames.
7. Within an epoch, a missed absolute range is never retried later, clamped, or
   rebased.
8. The transport layer remains payload-opaque and does not parse CIP, AMDTP, or
   PCM.
9. DICE primary and secondary streams share one presentation plan.
10. M-Audio uses the same common timeline and content-publication contract; its
    special handling remains device control, startup, wire timing conversion,
    and channel policy only.

The former 2,400-frame exposure theory is not part of this work.

## Implemented TX ownership model

### State machine

Each absolute producer slot has an explicit lifecycle that separates
stable wire geometry, mapping into an ASFW physical descriptor, and client PCM
finality:

```text
PlannedSilence
      |
      | backend builds CIP/DBC/SYT/disposition and initializes producer payload
      | planned DATA -> valid silence; cadence NO-DATA -> backend NO-DATA
      v
PayloadWritable ---------> PcmFilledWritable
      |                           |
      | IsochTxDmaRing maps       | IsochTxDmaRing maps
      | producer slot into        | the same producer slot
      | a physical descriptor     |
      v                           v
DmaMappedWritableSilence   DmaMappedWritablePCM
      |                           |
      | finalizedEnd              | finalizedEnd
      v                           v
FrozenSilence              FrozenPCM
      |                        |
      +-----------+------------+
                  |
                  v
               Completed
                  |
                  v
           ProducerReusable
```

Important properties:

- cadence, frame count, DBC, SYT, and DATA/NO-DATA disposition are fixed by the
  presentation plan before client content is required;
- a planned DATA packet begins with the backend's valid silent sample words;
- mapping a producer slot into the 48-entry ASFW physical descriptor ring does
  not by itself make its PCM final;
- a normal cadence NO-DATA slot remains NO-DATA and advances zero frames;
- if client PCM misses a planned DATA slot, that slot remains a silence-filled
  DATA packet and its absolute frame range is consumed; it is not converted
  late into cadence NO-DATA;
- late client fill changes PCM words only, not packet geometry or timeline
  state;
- transport never parses or edits CIP fields;
- audio never reads OHCI MMIO or command pointers;
- PCM words can change only through an accepted payload revision before the
  measured physical freeze guard;
- packet length, CIP, DBC, and SYT remain unchanged; transport may replace only
  the mutable tail's descriptor address with the matching image-1 tail;
- PCM words cannot change after freeze;
- the payload seal is created/validated at freeze, not at long-range planning;
- no missed client range is emitted later, retried, clamped, or rebased.

The selected implementation is complete alternate-image selection. Audio
never overwrites the image addressed by hardware. Image 0 is the valid armed
packet; image 1 is a complete alternative with an identical declared prefix.
Before finality, transport may select image 1 at initial bind or repoint the
single mutable-tail descriptor field. This is an ASFW mechanism and receives
no numerical safety proof from Apple's NuDCL implementation; hardware
acceptance must validate the two-packet guard.

### Neutral seam API

The implemented cross-service surface is:

```cpp
AcquireWritableSlot(packetIndex, image0)
PublishSlot(plannedPacket)
FinalizedEnd()
AcquireLatePayloadSlot(packetIndex, image1)
PublishLatePayload(packetIndex)
```

The write lease cannot be acquired below `finalizedEnd`. A publication racing
the advancing frontier is reported as too late. Transport owns mapping into the
48-entry physical ring, DMA synchronization, descriptor update, finality, seal,
and completion. The audio-side port sees only neutral opaque slots and cursors.

No hot-path allocation or blocking is permitted. All storage is preallocated
and generation-tagged. A write/freeze race resolves deterministically to
either complete content or complete silence.

### Independent frontiers

The engine and telemetry distinguish:

```text
plannedEnd        cadence/presentation plans exist
armedEnd          valid silence/NO-DATA producer slots are available
mappedEnd         physical descriptor programs reference those producer slots
contentFilledEnd  contiguous real-PCM payload fills were accepted
finalizedEnd      transport forbids new payload choices before this point
completionCursor  OHCI has retired packets before this point
```

Per-slot state remains authoritative because content may arrive with holes.
The contiguous frontiers are summaries, not substitutes for slot generations.

For DICE multi-stream output, fill of all channel slices for one common plan
must be coordinated. Either every stream freezes the real PCM for that frame
range, or every stream uses its matching silence payload. One stream must not
advance the shared plan with real content while its sibling transmits a stale
or differently timed range.

## Timing-property model after the split

### Output safety

Output safety expresses the conservative hardware-relative lead at which
newly published PCM can still reach its intended presentation slot.

It is derived from the implemented finality contract, including:

- the transport freeze boundary;
- packet granularity;
- one six-packet completion interval;
- the two-packet live-command repoint guard;
- a device-profile floor, 50 frames for the Duet at 48 kHz.

It is **not** derived from:

- the 8,192-frame HAL ring;
- the PCM cache capacity;
- the end of the cadence plan;
- the total packet-store capacity;
- historical scheduler stalls from a different implementation;
- a generic frame-alignment constant without a HAL contract.

### Output latency

Output latency remains the actual delay between the driver's content handoff
point and physical device presentation. It must be reported separately from
safety. Transfer delay must not be mechanically counted in both properties.

The Duet profile now reports Apple's 67 latency and 50 safety. This is exact HAL
accounting parity. Physical loopback measurement is still required to establish
whether equivalent delays live at equivalent points in the two drivers.

### Input timing

The generic input-safety override has been removed, so the Duet now reports the
original 40 latency plus 50 safety. Further input trace work is physical
validation, not a prerequisite for reproducing the original property table.
Useful measurements remain:

- acquisition presentation bus time;
- correlated host time;
- first and last frame copied into the HAL ring;
- publication time;
- current hardware sample coordinate at publication;
- safe readable lag and packet/batch granularity.

Any future measured correction must change the profile with evidence; graph
construction no longer manufactures a generic floor.

## Instrumentation and acceptance

Audio telemetry remains wire v6; Gate A2 does not reinterpret its fixed bytes.
The queue ABI is v9 and adds `finalizedEnd`, accepted/rejected late-rebind
counters, and the minimum command-pointer distance of an accepted rebind.

The bounded `TxCycleTraceRecord` continues to record plan coordinates, PCM
result, presentation time, disposition, preparation/publication/ownership/
completion cycles, and deadline headroom. `[TxFill]` now reports, every five
seconds:

```text
filled tooLate unavailable silentData cursor
finalized mapped committed rebound rejected minRebindDistance
```

Acceptance requires `rebound` to advance during real playback, `rejected=0`,
`minRebindDistance>=2`, `finalized<mapped`, and no payload-seal or ownership
fault. There are no per-packet logs. `[TxV3]` and `[TxFill]` are the coarse
liveness records; `[TxDeadline]`, `[TxOwnership]`, `[TxPayloadSeal]`,
`[TimelineEpoch]`, and `[ZTS]` remain anomaly-oriented.

## Validation plan

### Phase 1: establish current-path baselines — completed

The investigation completed these baseline tasks:

1. Run the current V3 stream at each supported rate.
2. Measure plan production, slot publication, refill, ownership, and completion
   distributions using current telemetry.
3. Test normal, CPU-loaded, UI-active, output-only, duplex, and RX-loss cases.
4. Identify whether any long delays remain and attribute them to a queue or
   service boundary.
5. Record the maximum and distribution; do not import the old 40–42-cycle
   observation into the result.

### Phase 2: host-test the new state machine — completed

Host tests cover:

- valid plan, arm, fill, freeze, completion, and reuse;
- payload fill immediately before and after freeze;
- stale token and wrong generation;
- silence-filled DATA selection when PCM is absent;
- irreversible absolute-frame advancement after silent DATA;
- normal cadence NO-DATA remaining distinct from missed-content silence;
- no cadence/DBC/frame-state double commit;
- wrap and slot reuse;
- DICE multi-stream all-or-nothing fill;
- M-Audio use of the common contract;
- teardown while armed or frozen;
- cross-service lifetime and generation changes.

Host tests cannot establish the OHCI fetch frontier or DMA visibility; that is
the remaining hardware acceptance boundary.

### Phase 3: focused hardware alternate-image acceptance — current

Use a development build, not a shipping runtime legacy selector:

1. Play continuous non-silent audio and confirm `[TxFill] rebound` advances.
2. Require `rejected=0`, `minRebindDistance>=2`, and a stable eight-slot
   `finalizedEnd-completionCursor` lead.
3. Capture the wire and require intact DATA payloads plus continuous cadence,
   DBC, and SYT. Image selection must never alter packet geometry.
4. Verify that a late fill loses to complete armed silence; no packet may mix
   the two images.
5. Repeat under load, output-only, duplex, buffer-size changes, restart, bus
   reset, and RX loss.

Pass criteria:

- every accepted fill appears intact on the wire;
- every rejected fill leaves complete valid silence intact;
- no packet contains a mixture of silence and partially written client PCM;
- no DBC/SYT/cadence discontinuity is introduced;
- only the mutable-tail descriptor address may change;
- no payload revision or seal failure occurs after freeze;
- the measured freeze margin has a conservative repeatable bound.

### Phase 4: verify reported and physical timing — current

1. Confirm Logic reads the 207-frame fixed total at 48 kHz.
2. Repeat all client buffer sizes to verify that fixed cost remains independent
   of the client I/O buffer.
3. Measure physical cable loopback RTL independently from reported properties.
4. Validate DICE profiles without replacing their device policy with Duet
   constants.

### Phase 5: remove the superseded contract — completed in code

The one-shot immutable publication boundary, prepared-target-derived safety,
generic input-safety floor, and selectable legacy behavior are gone. The
alternate-image/finality contract is the only path. Hardware rejection of the
two-packet guard requires revising that guard or contract; it does not authorize
silently restoring the old scheduler.

## Decision result and fallback boundary

### Gate A: direct mutation of the mapped image — not selected

ASFW does not mutate the payload image currently addressed by hardware. This
avoids the cross-service in-progress-write race entirely.

### Gate B: complete alternate-image selection — implemented

`IsochTxDmaRing` selects a separately completed image either at bind or by one
mutable-tail address store outside a two-packet live-command guard. Host proof
is complete; hardware acceptance of the guard is pending. AppleFWAudio's NuDCL
behavior supplies no numerical safety argument for this ASFW-specific choice.

### Gate C: alternate-image rebind fails hardware acceptance

Do not mutate hardware-visible payload memory speculatively. The alternative is
to keep a deep software presentation plan but expose only a measured short
hardware descriptor chain whose packets are finalized as they enter it.

If hardware rejects the two-packet guard, first increase the guard from measured
evidence. If no bounded rebind is safe, retain the deep software plan but expose
only a proven short hardware-final chain. Do not ship two timing engines.

## Open questions

1. Does the Duet accept the two-packet mutable-tail rebind guard without stale,
   mixed, or malformed wire payloads?
2. What is the minimum accepted rebind distance on the actual controller under
   load, restart, and bus-reset conditions?
3. What are the current V3 plan/refill/dispatch delay distributions after the
   recent architecture changes?
4. How should a DICE multi-stream plan commit content-versus-silence atomically
   across streams?
5. What portion of each backend's delay belongs in safety versus reported
   latency?
6. What is the Duet's physical cable-loopback RTL under ASFW versus Apple?

Questions 1, 2, and 6 require targeted hardware measurement. They are
deliberately not answered by analogy to NuDCLs, the old 40–42-cycle trace, or
host-only tests.

## Rejected or superseded explanations

The following are not valid premises for this work:

- a generic 2,400-frame SYT exposure;
- `WriteEnd` as a timing or scheduling event;
- the 8,192-frame ring or ZTS period as latency;
- nominal FireWire ticks per sample as a long-term clock;
- RX replay as the owner of HAL frame position;
- packet-store capacity as reported safety;
- AppleFWAudio's 800-position NuDCL capacity as 800 packets of immutable PCM;
- any direct numerical comparison between Apple's 800 NuDCL positions, ASFW's
  168 producer payload slots, and ASFW's 48 physical packet programs;
- treating a NuDCL command as if it were an ASFW OHCI descriptor;
- AppleFWAudio's private clip/DMA cursor names as proof of late payload fill;
- converting a missed planned DATA range into cadence NO-DATA when a valid
  silence-filled DATA packet preserves the planned wire geometry;
- the historical 40–42-cycle DriverKit stalls as a current V3 requirement;
- a completion timestamp used directly as presentation time;
- globally replacing device/backend latency policy with one generic constant.

## Immediate work order

1. Install the Gate A2 build and verify that HAL reads back the Duet's
   `50/67` output and `50/40` input split at 48 kHz.
2. Capture `[TxFill]` while playing audio and require `rebound` to advance,
   `rejected=0`, `minRebindDistance>=2`, and a finality-to-mapping separation.
3. Verify FireBug sees continuous DATA/NO-DATA cadence, DBC, and SYT with no
   malformed or stale payload after late rebinds.
4. Exercise startup, buffer-size changes, stop/start, bus reset, and RX-loss
   fallback. Require no payload-seal fault, transport underrun, or timeline
   rebase within an epoch.
5. Measure physical loopback RTL separately. Reported parity is not proof of
   converter or end-to-end parity.
6. Keep the metallic-artifact investigation deferred until the Apple-parity
   path has been accepted on hardware, per the explicit project decision.


## Implementation record — 2026-08-29

What was built against this document, what it measured, and which of the
document's own premises it corrected. Every number below is from the Duet on
this machine, read back through Logic or the driver log ring.

### Measured progression, 48 kHz

| stage | output fixed | input fixed | total | commit |
|---|---:|---:|---:|---|
| baseline | 896 | 256 | 1152 | `cf4c6c4d` |
| freeze at mapping frontier | 512 | 256 | 768 | `6ae514df` |
| reported latency derived | 451 | 168 | 619 | `76a93f2a` |
| latency corrected to measurement | 489 | 168 | 657 | `72204b91` |
| Gate A2 + exact Duet HAL policy | 117 | 90 | 207 | working tree |
| Apple's driver, same hardware | 117 | 90 | 207 | — |

Logic confirmed each stage to the frame. At a 32-sample buffer the round trip
went 25.3 ms -> 17.3 -> 14.2 -> **15.0** (output 19.3 -> 11.3 -> 10.1 -> **10.9**),
the last step being the correction upward described below. Predicted and observed
fixed costs agreed exactly at every buffer size and every stage.

**That agreement validates the model against itself, and nothing more.** Every
figure above is derived from ASFW's own timing model and confirmed by Logic
displaying what ASFW told it. No measurement in this record establishes the
*actual* delay between a sample being written and being heard.

### The reported numbers are known to be incomplete

Reported latency claims only the part the driver can prove. Specifically it
claims **zero** for the device's own analogue delay in both directions -- the DAC
after transmission and the ADC before acquisition -- because nothing host-side
can observe them. Apple's 67 output / 40 input presumably include those terms.
So ASFW under-reports by at least the converter delay, in a direction that makes
Logic align recorded material early.

Consistent with that, the developer reports that at 15.0 ms the perceived delay
is larger than the displayed figure. That is an unquantified subjective
impression, not a measurement, and it is recorded here as such -- but it points
the same way as the known omission, so it should not be dismissed either.

**The outstanding gap in the evidence chain is a loopback measurement**: a
physical cable from a Duet output to a Duet input, an impulse recorded with
Logic's Recording Delay at zero, and the offset read in samples. That single
number gives the true round trip, and true-minus-reported gives the unclaimed
delay, which can then be attributed instead of guessed. Until it exists, the
gap-to-Apple arithmetic below compares two possibly-incomplete numbers, and no
optimisation target derived from it is trustworthy to better than the size of
the omission.

### The contract change

`AcquireWritableSlot` / `PublishSlot` no longer means "final". A planned DATA
packet is armed with a complete silent image at plan time. Two payload images
per producer slot, ABI v9 generation markers, and independent `mappedEnd` and
`finalizedEnd` frontiers separate geometry publication, descriptor ownership,
payload choice, and completion.

A1 is merged (`84c85a5b`): the declared opaque prefix occupies descriptor 2 and
the mutable tail occupies descriptor 3. Gate A2 is now implemented: transport
scans already-bound packets from `CommandPtr+2`, acquire-checks image 1,
verifies that the declared prefixes match, publishes the new image to DMA, and
changes only descriptor 3's aligned `dataAddress`. The finality frontier is
`current absolute command position + 6 completion slots + 2 guard slots`.

There is no producer-side mutation of a hardware-addressed image. A fill/freeze
race resolves to complete image 0 or complete image 1; transport is the sole
writer of the live descriptor address. Host tests pin the guard behavior, the
single-field rebind, rejection without an invariant prefix, and monotonic
finality. Hardware still must confirm that the two-packet guard is sufficient.

Consequences: arming can no longer fail for content reasons, so the deferral
and convert-to-cadence-NO-DATA path was deleted with the contract that needed
it. A planned DATA packet always transmits as DATA, carrying content or its
armed silence, and consumes its frame range either way.

### Corrections to this document

**The 40–42 cycle stall figure was worse than assumed, not better.** Current-path
measurement gave 13 packets worst case on a quiet run — then 65 packets (8152 us)
on a loaded one. The document was right that it must be re-measured; it was
wrong to imply the answer would be smaller. Cutting the dispatch slack on the
strength of the quiet run would have made that stall an IT FATAL. The arm/freeze
split absorbs it instead, which is the strongest argument for the split over
tuning.

**The `[TxPrep]` telemetry the document asks for already existed and was dead.**
Fields, completed-interval mirrors, `Reset()`, snapshot copy and MCP export all
survived the V3 rewrite; the writers did not. Every reader had been shown zeros
since. Same for `txLastLeadTicks`. Restoring them was a prerequisite, not new
work.

**An eight-second plan-anchor defect was found and fixed** (`0292a010`), unrelated
to latency but corrupting the stream. An OUTPUT_LAST stamp carries only
cycleSeconds[2:0]; the lift into the full 128-second window corrected a
completion that appeared to *lead* its correlation but not one that appeared to
*lag*, so the ordinary publish race produced plan anchors exactly 8 s early in
bursts of ~6 packets every ~24 s. Window choice is now nearest-in-both-
directions, and the lift exists once instead of twice.

**The measured ASFW presentation lead was 105 frames; the Duet profile now
reports Apple's 67-frame policy.**
`[TxLead]` measures the stamped presentation lead at 53876 ticks (min 105, max
111 frames). The duplex path stamps `transmitBusTicks + replayEntry.sytOffset +
transfer`, and the recovered offset carries a full 16-cycle modulus whenever the
device's raw SYT offset falls below the transfer delay. Linux does the identical
thing including the wrap (`amdtp-stream.c:483-487`) and our 12800-tick transfer
delay matches its blocking-mode derivation at `:288-292` exactly, so this is
ordinary AMDTP behaviour. An earlier suspicion of a double-counted transfer
delay was unfounded: RX subtracts and TX adds back, symmetric with Linux.

The 105-frame observation remains useful wire-timing telemetry, but it is not a
HAL latency derivation. The current project decision is to reproduce the
original Duet personality first: the profile reports 67 output frames while the
wire planner keeps the Linux-compatible SYT/transfer construction unchanged.
This closes accounting parity but makes the need for a physical loopback
measurement more explicit, not less.

### Reported parity is closed; physical parity remains unmeasured

At 48 kHz the model now reports the same fixed split as Apple's Duet driver:

```text
output = safety 50 + latency 67 = 117
input  = safety 50 + latency 40 =  90
total                              207 frames
```

The earlier 450-frame accounting gap is therefore closed in code: 78 frames by
removing the generic input-safety override, 334 by moving output finality from
54 to 8 cycle slots and honoring the 50-frame profile floor, and 38 by applying
the original 67-frame output-latency policy. None of those arithmetic changes
constitutes a loopback measurement.

### Open

The metallic artifact remains unexplained and is deliberately deferred. It is
not a gate for implementing or measuring Apple parity. After hardware
acceptance, `[TxFill]` can still distinguish successful late rebinds from
silence substitutions without changing the scheduling contract.

The exact acceptance capture is:

```bash
log stream --style compact --info --debug --predicate 'process == "kernel" AND (eventMessage CONTAINS "[TxV3]" OR eventMessage CONTAINS "[TxFill]" OR eventMessage CONTAINS "[TxOwnership]" OR eventMessage CONTAINS "[TxPayloadSeal]" OR eventMessage CONTAINS "[TxDeadline]" OR eventMessage CONTAINS "[TimelineEpoch]" OR eventMessage CONTAINS "[ZTS]")' > /tmp/asfw-rtl.log
```

Run it from the Codex terminal with the `!` prefix while audio is playing.
