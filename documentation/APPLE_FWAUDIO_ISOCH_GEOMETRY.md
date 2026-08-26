# AppleFWAudio.kext — Isochronous Geometry Reference

Decompilation findings from Apple's original FireWire audio driver, extracted
2026-08-26. This is a **behavioural reference**, not a design to port. Nothing
here was copied; see the *References are read-only* rule in `CLAUDE.md`.

**Provenance.** `AppleFWAudio` (Mach-O x86, 2.1 MB) via
`~/Desktop/FWA_KEXT_CLEAN/AppleFWAudio-update.i64`, IDA 9.x + Hex-Rays.
Addresses below are file offsets in that image. Symbols are Apple's own C++
mangled names; the binary retains `FireLog` format strings, which is where most
of the named constants come from.

---

## 1. How a callback becomes an interrupt

Every packet in a DCL program is one OHCI descriptor block. The only thing that
sets `IRQ_ALWAYS` on a block's `OUTPUT_LAST` / `INPUT_LAST` is a
`kDCLCallProcOp` (legacy) or `IOFWDCL::setCallback` (NuDCL). Every other packet
descriptor is `NO_IRQ`.

So **callback cadence == interrupt cadence, exactly.**

Descriptor control-word semantics cross-checked against Linux
`references/linux-ohci-firewire-low-level-stack/ohci.h:56-70`:

```
DESCRIPTOR_OUTPUT_LAST  (1 << 12)      DESCRIPTOR_IRQ_ALWAYS   (3 << 4)
DESCRIPTOR_INPUT_LAST   (3 << 12)      DESCRIPTOR_NO_IRQ       (0 << 4)
```

DCL opcodes confirmed against Apple's own header
`IOFireWireFamily.kmodproj/IOFireWireFamilyCommon.h:768-788`:

| op | name | op | name |
|----|------|----|------|
| 1  | `kDCLSendPacketStartOp`    | 9  | `kDCLLabelOp` |
| 3  | `kDCLSendPacketOp`         | 10 | `kDCLJumpOp` |
| 5  | `kDCLReceivePacketStartOp` | 12 | `kDCLUpdateDCLListOp` |
| 6  | `kDCLReceivePacketOp`      | 13 | `kDCLTimeStampOp` |
| 8  | `kDCLCallProcOp`           | 20 | `kDCLNuDCLLeaderOp` |

---

## 2. The constants

From `AM824DCLWrite::ReInit` (0x397a8) and `AM824DCLRead::ReInit` (0x324ca) —
the else-branch defaults, corroborated by the two mismatch warnings that name
them explicitly:

```
kDefaultNumBufferGroups     = 100
kDefaultNumPacketsPerGroup  = 8
kCallbackTimeoutInMSec      = 20
```

`NumBufferGroups` and `NumPacketsPerGroup` are **IORegistry property keys**
(strings at 0x69fe4 / 0x6a014), so a device personality can override them.
100/8 are the fallbacks.

Program depth = 100 x 8 = **800 packets = 800 isoch cycles = 100 ms**, closed
into a ring by `kDCLJumpOp` / `IOFWDCL::setBranch`.

---

## 3. Interrupt cadence — the headline numbers

| path | where `IRQ_ALWAYS` lands | packets/IRQ | IRQ/s |
|------|--------------------------|-------------|-------|
| legacy DCL TX & RX | `CallProc` once per buffer group | 8 | **1000** |
| NuDCL RX | `setCallback` when `(group+1) % 20 == 0` | 160 | **50** |
| NuDCL TX | `setCallback` on the **last NuDCL of the lap only** | 800 | **10** |
| *ASFW today* | *every completion group* | *6* | *1333* |

The NuDCL RX divisor is computed, not hardcoded
(`AM824NuDCLRead::SetupReceiveBuffer`, 0x3d484):

```c
buffersPerCallback = 0x4E20 / (125 * fNumPacketsPerGroup);   // 20000us / (125us * 8) = 20
```

i.e. `kCallbackTimeoutInMSec * 1000 / (cycle_us * packetsPerGroup)`. **Apple
derives the interrupt cadence from a 20 ms time budget, not a packet count.**
Guarded by `fNumBufferGroups % buffersPerCallback == 0` (100 % 20 = 0). Note
the warning string states this backwards relative to the code.

On TX (`AM824NuDCLWrite::SetupSendBufferForExternalSync`, 0x45012) the callback
is set *outside both loops* — one interrupt per 100 ms lap.

**How Apple still gets per-packet timing:** every packet NuDCL gets
`IOFWDCL::setTimeStampPtr`. The DMA engine writes the timestamp into a host
array with no interrupt and no CPU. Reading it later is free. An interrupt says
*there is work to do*; a timestamp says *what time it is*. Apple answers those
with two different mechanisms.

TX refill is driven by CoreAudio's clip path plus `AppleFWAudioIsocEngine`'s
work timer (`scheduleDelayedWork` / `s_WorkTimerHandler`), not by isoch
interrupts. Losing an isoch interrupt costs a lap marker, not the stream.

---

## 4. Per-rate geometry — the two families are genuinely different

`AM824DCLWrite::SetupSendBuffer` (0x367a8) dispatches on exact sample rate;
anything not listed returns `-50` (paramErr):

| rate | builder | arg |
|------|---------|-----|
| 32000 / 48000 / 96000 / 192000 | `SetupSendBufferFor8NkHz` | — |
| 44100 | `SetupSendBufferForMultipleOf44100Hz` | 441 |
| 88200 | same | 882 |
| 176400 | same | 1764 |

### 8N family (0x36852)

```c
framesPerPacket = sampleRate / 8000;                 // 48k -> 6, constant
packetQuadlets  = framesPerPacket * sequences + 2;   // +2 = CIP header
```

### 44.1 family (0x3701c) — phase accumulator, per packet

```c
acc += mult % 80;   frames = mult / 80;
if (acc > 79) { acc -= 80; frames++; }
```

At `mult = 441`: 5 frames per packet, 6 when the accumulator carries. Average
441/80 = **5.5125** = 44100/8000. This is **non-blocking 5/6** — variable
packet size, no NO-DATA packets. Matches the cadence analysis in
`documentation/44100.md`.

### NuDCL path — blocking, both families

`SetupSendBufferForExternalSync` is the only substantive builder in the NuDCL
write class (the `For8NkHz` and `ForMultipleOf44100Hz` overrides are 7- and
10-byte stubs, so rate handling moved inside it):

```c
packetSize = 4 * SYT_INTERVAL * (audioSequences + midiSequences);
```

Fixed size per SYT_INTERVAL block = **blocking mode**.

| rate | 32k | 44.1k | 48k | 88.2k | 96k | 176.4k | 192k |
|------|-----|-------|-----|-------|-----|--------|------|
| SYT_INTERVAL | 8 | 8 | 8 | 16 | 16 | 32 | 32 |

**Legacy DCL is non-blocking; NuDCL is blocking. ASFW is blocking, so NuDCL is
the path that corresponds to ours.**

### Host sample ring

`AM824DCLRead::SetUpSampleBuffer` (0x32e2c):

| sample rate | frames |
|-------------|--------|
| <= 48000 | 6400 (133 ms @48k) |
| 88.2k / 96k | 12800 |
| 176.4k / 192k | 25600 |

---

## 5. What a "buffer group" is — and what it is not

**It is not a FireWire concept, not an OHCI concept, and not even an
IOFireWireFamily concept.**

```
grep -ril "buffergroup" IOFireWireFamily.kmodproj/   ->   0 files
```

The only files matching "group" at all are `IOFWWorkLoop`, `IOFireWireDevice`
and `FWTracepoints`, in unrelated senses. IOFireWireFamily offers exactly one
structure: a flat singly-linked list of DCL commands. There is no chunking
primitive.

"Buffer group" is a term **AppleFWAudio invented for itself**, meaning:

> every 8th command in my list I also hang a `TimeStamp` + `UpdateDCLList` +
> `CallProc`, and I keep a small struct describing that run.

### The struct

Offsets **inferred from observed writes**, not from a header (no header exists):

| offset | field | purpose |
|--------|-------|---------|
| +0x0C | `pDCLCallProc` | this group's callback command |
| +0x10 | `pDCLTimeStamp` | its timestamp command |
| +0x14 | first packet DCL | start of the run |
| +0x18 | last packet DCL | end of the run |
| +0x20 | `updateList` | array of descriptors to re-commit |
| +0x24 | `updateListCount` | how many |
| +0x28 | `groupIndex` | which chunk of the ring |
| +0x38 | `nanosPerFrame` (u64) | `1e9 / sampleRate` |

NuDCL variants: 48 bytes (RX) / 56 bytes (TX), with `[+0]` = back-pointer to
the owning object, `[+4]` = group index, `[+8]` = `nanosPerFrame`.

### What it was for

`DVCreatePlayBufferGroupUpdateList` (0x35ea0) walks the group's DCL chain,
keeps only commands with opcode **1 or 3** (`SendPacketStart` / `SendPacket` —
the buffer-carrying ones), and stores that filtered array in the group. The
group's trailing `kDCLUpdateDCLListOp` then points at it: *"re-commit exactly
these N descriptors."* Computed once at setup, never recalculated.

And the callback receives the group struct directly as its refcon
(`AM824NuDCLRead::s_NuDCLCallback`, 0x3ed24):

```c
int s_NuDCLCallback(void *refcon) {
    return refcon->owner->NuDCLCallback(refcon);   // [+0] is the back-pointer
}
```

So the ISR path is: bell rings -> you are handed the work order -> it already
names the chunk, the frame/time scale, and the exact descriptor list to flush
-> do it -> return. **Zero discovery work in interrupt context**, on 2000-era
CPUs, in a linked list where "find the buffers in chunk 37" would otherwise
mean traversing the chain.

The group is also the driver's unit of *position*: `fCurrentBufferGroupIndex`,
`fLastBufferGroupIndex`, `fSampleCounter`, and the whole decode path
(`DecodePacket`, `HandleBufferWrap`,
`HandleProcessCIPHeadersFirstTimeProcesssing`) takes a
`PerReadBufferGroupDataStruct*`. Ring wrap is handled at group granularity.

### Why ASFW does not need it

The group exists to avoid pointer-chasing a linked list. We have a flat
descriptor array; "which descriptors are in chunk 37" is `index % ringSize`.
The refcon problem does not exist — we already hand the context object to the
handler. The update list does not exist — we write descriptors directly and
`IoBarrier`. **All three of the group's jobs are free in our design.**

What survives the translation is one idea only: **Apple's bookkeeping chunk
(8 packets) and its interrupt spacing (160 / 800 packets) are different
numbers.** That independence is transferable. The struct is not.

---

## 6. Caveats

- **Two lineages in one binary.** The legacy DCL path is classic-Mac "DV"
  streaming code (`DVAllocatePlayBufferGroup`, `IOMallocAligned`, fixed 100x8
  rings) written ~2000-2003. Its 1 ms interrupt cadence was tuned for that
  era's CPUs. The NuDCL path is the later one and is the relevant comparison.
- The existing Apple comparison block in `AudioTimingGeometry.hpp:300-345`
  describes only the **legacy** path and concludes ours is "in range" at
  0.75 ms. That premise is superseded: the NuDCL path is 20 ms / 100 ms.
  **That comment needs correcting.**
- Struct offsets in section 5 are inferred from decompiled writes. The opcode
  values and the absence of any group concept in IOFireWireFamily are
  confirmed against Apple headers.
