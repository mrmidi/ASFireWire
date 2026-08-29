# ASFW Round-Trip Latency and TX Content Ownership

Status: design and evidence record, 2026-08-29
Source tree inspected: `cf4c6c4d4c6d`
Implementation status: see **Implementation record** at the end (through `72204b91`).

This document records the latency investigation that followed Audio Engine V3.
It separates measured facts, reverse-engineering evidence, current ASFW
contracts, design conclusions, and remaining hardware questions. It is not a
replacement for `AUDIOENGINEV3_DRAFT.md`; it narrows the next work to the point
where V3 currently turns transport preparation depth into audible latency.

## Executive conclusion

The V3 hardware timeline, absolute sample coordinates, 8,192-frame HAL/cache
geometry, and backend-specific SYT conversion remain the right foundation.

The main output-latency defect is a different contract:

```text
current ASFW

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

The next V3 step should therefore model a stable planned producer slot
separately from its writable PCM payload, its mapping into an ASFW OHCI
descriptor, and its physical hardware-fetch boundary. The preferred steady-state
path is late PCM fill into a preallocated, silence-initialized DATA payload
whose ASFW OHCI address and length remain unchanged. A separate-buffer/remap
design is an ASFW-specific fallback, not an inference from NuDCL behavior. HAL
output safety must be derived from the measured physical content-freeze
boundary, not from the long-range plan cursor.

No current design decision is justified by the historical 40–42-cycle
DriverKit stall observation. That number came from an older architecture and
must not be treated as a V3 scheduling distribution or hardware requirement.
Current-path telemetry must establish the required planning and transport
depths anew.

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

### ASFW comparison at 48 kHz

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

The large ASFW safety value is not merely an incorrect property value. Under
the current immutable-packet contract, PCM really is committed approximately
that far ahead, so reporting a small safety value would be false.

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

## Current ASFW contract at `cf4c6c4d4c6d`

### Planning depth becomes safety

`AudioTimingGeometry` currently defines:

```text
hardware/ownership slots = 48
dispatch slack slots     = 72
prepared target          = 120
shared packet slots      = 168
```

These are current policy values, not established hardware minima. In
particular, the comment tying the 72-slot allowance to historical 40–42-slot
stalls records old evidence and must not be used as a present V3 acceptance
criterion.

`AudioGeometryPolicy::RequiredOutputSafetyFrames` converts all 120 prepared
cycle slots into sample frames, adds transfer and packet allowances, and rounds
the result to a 32-frame boundary. At 48 kHz this dominates the reported output
safety.

The 32-frame alignment is also not an established safety-offset contract. The
original Duet driver reports a 50-frame safety offset at 48 kHz. Ring-buffer
alignment and reported safety must not be coupled without an independent HAL
requirement.

### Publication means permanent immutability

`IAmdtpTxSlotProvider` exposes only:

```cpp
AcquireWritableSlot(packetIndex, slot)
PublishSlot(packet)
```

`DiceTxStreamEngine::PrepareTransmitSlot` obtains PCM, builds the complete wire
packet, publishes it, and commits packetizer cadence. The isoch transport then
assumes the payload is immutable until completion. It seals the opaque bytes,
checks the seal at completion, and treats any mutation as a fatal producer
contract violation.

This contract is internally consistent, but it forces the content lead to be
at least the complete prepared/owned transport horizon.

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

The current `commitGeneration` and `payloadSeal` turn the audio-to-transport
handoff into a one-shot final commit. That is the precise contract V3 latency
work must change. It must not change the content/transport layer boundary or
pretend a NuDCL command is an ASFW OHCI descriptor.

### OXFW timing is currently placeholder policy

`OxfwProfileBuilder` currently assigns every timing entry:

```text
input latency  128
output latency 128
input safety   128
output safety   64
```

Those values do not reproduce the exact Duet override table. Output-path
ownership must be fixed before reporting a smaller safety offset, and input
latency/safety needs its own measurement. The original values are targets and
cross-checks, not permission to report unimplemented performance.

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

## Proposed TX ownership model

### State machine

Each absolute producer slot should have an explicit lifecycle that separates
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
      | measured physical         | measured physical
      | fetch/freeze point        | fetch/freeze point
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
- the payload address, length, CIP, DBC, and SYT remain unchanged during the
  preferred stable-mapping revision;
- PCM words cannot change after freeze;
- the payload seal is created/validated at freeze, not at long-range planning;
- no missed client range is emitted later, retried, clamped, or rebased.

There are two ASFW implementation candidates for a writable producer payload:

1. **Stable mapped-payload fill:** an ASFW physical descriptor already
   references the preallocated silence-initialized producer slot. Audio/Wire
   overwrites its PCM words before a proven hardware fetch frontier; transport
   republishes payload DMA visibility without changing descriptor address or
   length.
2. **Alternate producer-slot selection:** Audio/Wire finalizes a different
   preallocated payload image and `IsochTxDmaRing` maps/selects it before a
   separately proven physical descriptor boundary.

The first matches the recovered steady-state behavior at both Apple layers:
compiled descriptors continue to address the same packet buffer while client
PCM changes only that buffer's words. It is preferred only if ASFW proves that
DriverKit DMA publication and the OHCI fetch boundary make the write safe. The
second is an ASFW fallback and receives no safety proof from Apple's NuDCL
implementation. Neither may be assumed safe from host tests alone.

### Neutral seam API sketch

The exact names are provisional, but the cross-service contract needs the
equivalent of:

```cpp
struct TxPayloadSlotToken {
    uint64_t generation;
    uint64_t packetIndex;
};

enum class TxPayloadFillResult {
    Filled,
    TooLate,
    AlreadyFrozen,
    AlreadyFilled,
    WrongGeneration,
    InvalidSlot,
};

PublishPlannedSlot(packetIndex, silenceInitializedOpaquePacket)
AcquirePayloadWrite(token) -> bounded opaque write lease or scratch view
CommitPayloadWrite(token) -> TxPayloadFillResult
```

These names are deliberately provisional. If the stable-mapping design wins,
the write lease must be impossible to acquire at or behind the transport's
physical freeze guard, and payload revision commit must cause transport-owned
DMA publication before the slot can freeze. If the alternate-slot design wins,
`CommitPayloadWrite` selects the complete alternate image. The transport
implementation owns mapping into the 48-entry physical ring, DMA
synchronization, optional descriptor update, freeze decision, and completion.
The audio-side port sees only neutral tokens, opaque buffers, and results.

No hot-path allocation or blocking is permitted. All storage is preallocated
and generation-tagged. A write/freeze race must resolve deterministically to
either complete PCM or complete silence; hardware must never observe a partial
mix. If alternate-slot storage is required, its capacity cost must be
measured before fixing the final slot count.

### Independent frontiers

The engine and telemetry should distinguish at least:

```text
plannedEnd        cadence/presentation plans exist
armedEnd          valid silence/NO-DATA producer slots are available
mappedEnd         physical descriptor programs reference those producer slots
contentFilledEnd  contiguous real-PCM payload fills were accepted
freezeCursor      physical fetch guard forbids payload writes before this point
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

Output safety should express the conservative hardware-relative lead at which
newly published PCM can still reach its intended presentation slot.

It is derived from measured current-path behavior, including:

- the transport freeze boundary;
- packet granularity;
- cross-service publication/revision delay;
- backend-specific handoff requirements that genuinely constrain the final
  content decision.

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

The Apple Duet split of 67 latency and 50 safety is useful evidence about the
original driver's semantics, but ASFW must measure where equivalent delays live
in its own pipeline.

### Input timing

The TX ownership change does not solve the current input excess. Input needs a
separate trace from hardware acquisition through packet decode and ADK-ring
publication. Required measurements are:

- acquisition presentation bus time;
- correlated host time;
- first and last frame copied into the HAL ring;
- publication time;
- current hardware sample coordinate at publication;
- safe readable lag and packet/batch granularity.

The resulting measured safe lag becomes input safety. Acquisition-to-delivery
delay becomes input latency. The Duet's exact 48-kHz reference split is 40
latency plus 50 safety.

## Instrumentation required before tuning

The existing V3 telemetry records plan publication, ownership, and completion,
but the proposed contract requires new events and distributions.

### Per-slot trace fields

Extend the bounded cycle trace with:

- plan/arm cycle and baseline disposition;
- payload-fill request, start, and completion cycles;
- payload-fill result and generation;
- freeze cycle;
- selected silence/PCM state;
- presentation cycle and bus ticks;
- fill-to-freeze headroom;
- freeze-to-presentation headroom;
- completion cycle;
- producer-slot index, physical-ring index, and command-pointer distance as
  reported by transport;
- stable-mapped versus alternate-slot mode;
- synchronized payload identity or test nonce.

If the fixed telemetry structure changes, increment the telemetry wire version
and update the C++, Swift, MCP, and fixed-size checks together. Do not reinterpret
existing v6 bytes under new field meanings.

### Counters and histograms

Add bounded counters/histograms for:

- arm depth;
- current-path plan and refill execution delay;
- accepted payload fills and fill duration;
- `TooLate`, `AlreadyFrozen`, generation, and invalid-slot rejections;
- planned DATA packets transmitted as silence because PCM was absent;
- planned DATA packets transmitted as silence because fill was late;
- normal cadence NO-DATA packets, counted separately from silent DATA;
- freeze lead in cycles and frames;
- fill-to-freeze and freeze-to-completion distance;
- fills accepted before versus after physical mapping;
- stable-mapped and alternate-slot attempts;
- payload DMA-publication and optional descriptor-update failures;
- DICE grouped-fill failures;
- wire-payload nonce mismatches.

The historical 40–42-cycle figure may be retained only as labeled archival
context. New histograms, under the current implementation and workload, decide
the planning and armed-depth policies.

### Logging policy

Do not add per-packet logs. Keep one coarse liveness/margin heartbeat and emit
anomaly-only records with first-occurrence plus power-of-two repetition
limiting. Suggested tags:

- `[TxArm]` for inability to maintain valid baseline packet coverage;
- `[TxPayload]` for late, rejected, or inconsistent payload fills;
- `[TxFreeze]` for freeze-margin anomalies;
- `[TxWireCheck]` for payload identity mismatches;
- existing `[TxV3]`, `[TxDeadline]`, and `[TxOwnership]` summaries.

## Validation plan

### Phase 1: establish current-path baselines

Before choosing a new depth:

1. Run the current V3 stream at each supported rate.
2. Measure plan production, slot publication, refill, ownership, and completion
   distributions using current telemetry.
3. Test normal, CPU-loaded, UI-active, output-only, duplex, and RX-loss cases.
4. Identify whether any long delays remain and attribute them to a queue or
   service boundary.
5. Record the maximum and distribution; do not import the old 40–42-cycle
   observation into the result.

### Phase 2: host-test the new state machine

Host tests must cover:

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

Host tests cannot establish the OHCI fetch frontier or DMA visibility.

### Phase 3: focused hardware payload-fill experiment

Use a development build, not a shipping runtime legacy selector:

1. Publish planned DATA producer slots with a recognizable valid-silence nonce
   while keeping packet length, CIP, DBC, and SYT stable.
2. Confirm through transport trace that `IsochTxDmaRing` has mapped the selected
   producer slot into a known physical descriptor-ring index.
3. Overwrite only the PCM words with a different per-slot nonce or waveform at
   progressively smaller distances from the actual OHCI command pointer.
4. Republish payload visibility using the transport's DMA primitives and
   barriers without changing the physical descriptor address or length.
5. Capture the actual wire packet and correlate its nonce with the accepted
   payload-fill result, producer slot, physical-ring index, and command-pointer
   distance.
6. Reject fills before entering any distance where mixed, stale, or
   nondeterministic payloads appear.
7. Verify separately that planned cadence NO-DATA packets are unaffected.
8. Repeat across rates, packet sizes, duplex/output-only operation, bus reset,
   and load.
9. Only if stable mapped-payload fill cannot pass, run a distinct ASFW
   alternate-slot/descriptor-selection experiment.

Pass criteria:

- every accepted fill appears intact on the wire;
- every rejected fill leaves complete valid silence intact;
- no packet contains a mixture of silence and partially written client PCM;
- no DBC/SYT/cadence discontinuity is introduced;
- physical descriptor address/length remain unchanged in stable-mapped mode;
- no payload revision or seal failure occurs after freeze;
- the measured freeze margin has a conservative repeatable bound.

### Phase 4: derive and verify timing properties

After the freeze contract is proven:

1. Set output safety from the conservative content-freeze lead, not the plan
   horizon.
2. Measure output latency separately.
3. Compare Logic's fixed cost with the original-driver reference on the same
   Duet.
4. Validate the Saffire/DICE profile without generic inflation.
5. Instrument and correct the input path separately.
6. Repeat all client buffer sizes to verify that fixed cost remains independent
   of the client I/O buffer.

### Phase 5: remove the superseded contract

Once hardware validation passes:

- replace `AcquireWritableSlot`/one-shot immutable `PublishSlot` with the
  planned-geometry/payload-fill/freeze contract;
- move the integrity seal to payload freeze;
- remove the prepared-target-derived safety calculation;
- remove stale comments and tests that equate queue depth with safety;
- do not retain a selectable legacy scheduler or snapshot fallback;
- update `AUDIOENGINEV3_DRAFT.md` with exact Duet numbers and the verified
  content-freeze result.

## Decision gates and alternatives

### Gate A: stable mapped-payload PCM fill is safe

If ASFW can prove a deterministic write boundary after a producer slot has been
mapped into the physical ring, retain deep cadence/silence coverage and fill
PCM words late without descriptor-address or length changes. Choose final
planning, mapped, freeze, and content leads from current telemetry and hardware
measurements.

### Gate B: stable mapped fill is unsafe, alternate-slot selection is safe

Use a separately finalized preallocated producer slot and make
`IsochTxDmaRing` select it before a proven physical descriptor boundary. Prove
that boundary independently. AppleFWAudio's NuDCL behavior supplies no safety
argument for this ASFW-specific alternative.

### Gate C: neither hardware-visible update is safe

Do not mutate hardware-visible payload memory speculatively. The alternative is
to keep a deep software presentation plan but expose only a measured short
hardware descriptor chain whose packets are finalized as they enter it.

That alternative is viable only if current-path scheduling measurements show
that the short chain survives required workloads. Historical stall figures do
not decide the result. Pick one proven contract and remove the other; do not
ship two timing engines.

## Open questions

1. How far ahead can this OHCI implementation fetch a descriptor or payload?
2. Can ASFW safely fill PCM words in an already-mapped, stable-address and
   stable-length payload,
   and which DriverKit DMA synchronization is sufficient?
3. What are the current V3 plan/refill/dispatch delay distributions after the
   recent architecture changes?
4. What conservative margin is required above the observed freeze boundary?
5. If stable mapped fill fails, can ASFW select a separately finalized buffer
   before branch publication or another proven fetch boundary?
6. How should a DICE multi-stream plan commit PCM-versus-silence atomically
   across streams?
7. What portion of each backend's delay belongs in safety versus reported
   latency?
8. What accounts for the current OXFW input latency and safety separately?
9. Does AppleFWAudio's `synchronizeWithIO` serve only callback exclusion on the
   target hardware, or does another unobserved platform layer add cache/DMA
   synchronization?

Questions 1, 2, 4, 5, and 9 require targeted hardware or platform research.
They are deliberately not answered by analogy to NuDCLs, the old 40–42-cycle
trace, or host-only tests.

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

1. Correct the exact Duet reference values in `AUDIOENGINEV3_DRAFT.md`.
2. Add current-path scheduling/refill distributions without changing TX
   behavior.
3. Record the completed AppleFWAudio, AppleFWOHCI, and Saffire content/transport
   layer analysis. Selector `3` is resolved as
   `kFWNuDCLModifyNotification`; no further NuDCL work is required for the
   stable-mapped experiment.
4. Specify and host-test the neutral planned/writable/frozen slot contract.
5. Implement the stable mapped-payload hardware nonce experiment and establish
   the payload-write frontier against ASFW's actual physical command pointer.
6. Replace the current one-shot immutable publication contract if the hardware
   gate passes.
7. Derive output safety and latency from the new measured boundaries.
8. Instrument and correct input latency independently.

This ordering preserves the working V3 clock architecture, avoids retuning
against stale scheduling evidence, and tests the only part that cannot be
proven from source or host simulation: when a future OHCI payload becomes
physically irreversible.


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
packet is armed with a complete silent image at plan time; its samples stay
writable until transport binds the slot to a descriptor. Two payload images per
producer slot (ABI v7), a generation-tagged readiness marker, and `mappedEnd`
publishing the freeze frontier back to the producer.

This is **Gate A's stable-mapped design minus the hardware question**: the
freeze frontier is a software cursor (the mapping frontier), not the physical
fetch boundary. No payload the hardware may be fetching is touched, so the
open questions 1, 2 and 4 remain open.

There is no torn-packet window. Transport is the sole writer of the descriptor
and selects the image before binding, so it never observes a half-written one;
a fill that loses the race is indistinguishable from one that never happened.
The document's worry that a write/freeze race "must resolve deterministically"
is satisfied by construction here, without needing the alternate-slot fallback.

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

**Output reported latency is 105 frames, not the ~67 the reference implies.**
`[TxLead]` measures the stamped presentation lead at 53876 ticks (min 105, max
111 frames). The duplex path stamps `transmitBusTicks + replayEntry.sytOffset +
transfer`, and the recovered offset carries a full 16-cycle modulus whenever the
device's raw SYT offset falls below the transfer delay. Linux does the identical
thing including the wrap (`amdtp-stream.c:483-487`) and our 12800-tick transfer
delay matches its blocking-mode derivation at `:288-292` exactly, so this is
ordinary AMDTP behaviour. An earlier suspicion of a double-counted transfer
delay was unfounded: RX subtracts and TX adds back, symmetric with Linux.

This sharpens the document's warning about copying the Duet override table.
Apple reports 67 total while presumably stamping a comparable lead, so **their
anchor sits at a different point in the path than ours**. `HardwareSampleTimeline`
anchors on TX completion, so everything after transmission — including those 105
frames — falls outside the safety offset and must be reported. The reference
split is not transferable without matching the anchor.

### The whole remaining gap is the safety offsets

Remaining gap to Apple is 450 frames, and 412 of it -- 92% -- is the two safety
offsets. Input *latency* is already at parity (40). There is no accounting trick
available: what a safety offset measures is how early content must be final, and
the only way to shrink one is to make content final closer to the wire.

Note in particular that **moving the anchor is not a latency win.** Logic shows
safety + latency + buffer, and that sum must equal physical reality wherever the
anchor sits: anchoring on presentation would place the anchor 105 frames later,
so safety would have to grow by the same 105 to keep content final at the same
physical instant. It is worth doing to make our split directly comparable to the
reference's -- right now the two cannot be compared term by term -- but it buys
no milliseconds.
1. **Gate A proper** — freeze closer than the mapping frontier. The single
   largest item at ~280 frames, and the only one that shrinks output safety at
   all. Content is currently final one descriptor runway (48 slots) plus one
   refill batch (6) before transmission; Apple's 50-frame safety means theirs is
   final ~6-8 packets out. Still needs the nonce experiment. Prerequisite A1 (splitting the
   descriptor at an opaque payload prefix so re-pointing an image is a single
   aligned store rather than two) is written and parked, unmerged, because it is
   wire-observable and would confound an open audio-quality investigation.
2. **Input safety, 128.** Worth 78 frames. The derived floor is 104 — one 40-frame interrupt batch
   (observed acquisition ages 15–30) plus a 64-frame cushion — which 32-frame ring
   alignment rounds back to 128. Lowering it needs RX-side jitter numbers that do
   not exist yet; `[TxPrep]`'s 8 ms outliers are the only tail measurement we
   have and they argue against optimism.

### Open

A metallic artifact appeared after the freeze-at-mapping change and is not yet
explained. TX scheduling telemetry is clean at the time of observation (263k
wakes, max 208 us, margin pinned, no seal mismatches, no `noCycleAnchor`), RX
reports `receivingData` with the expected 3:1 DATA/NO-DATA ratio and zero replay
resets, and arm-then-fill encoding is pinned byte-identical to one-shot
encoding by host test. That leaves coverage — packets transmitting their armed
silence — as the surviving hypothesis, which `[TxFill]` (`46976c32`) was added to
measure and which has not yet been read back.
