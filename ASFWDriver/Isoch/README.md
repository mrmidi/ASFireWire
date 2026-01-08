# Isochronous Stack

The Isoch stack handles **bidirectional real-time audio streaming** over IEEE 1394 isochronous channels, implementing IEC 61883-1 (CIP) and IEC 61883-6 (AM824) protocols.

**Status:** ✅ Receive (IR) production-ready @ 8000 pkts/sec, 0% drops | 🚧 Transmit (IT) in simulation mode

---

## Quick Navigation

| Component | File | Purpose |
|-----------|------|---------|
| **IR Context** | [IsochReceiveContext.cpp](IsochReceiveContext.cpp) | OHCI IR DMA manager, interrupt handling |
| **IT Context** | [Transmit/IsochTransmitContext.hpp](Transmit/IsochTransmitContext.hpp) | IT simulation (awaiting DMA integration) |
| **DMA Manager** | [Memory/IsochDMAMemoryManager.cpp](Memory/IsochDMAMemoryManager.cpp) | Dual-slab allocator (descriptors + payloads) |
| **Stream Parser** | [Receive/StreamProcessor.hpp](Receive/StreamProcessor.hpp) | CIP parsing, DBC validation, metrics |
| **Packet Builder** | [Encoding/PacketAssembler.hpp](Encoding/PacketAssembler.hpp) | TX packet assembly (CIP + AM824) |
| **CIP Decoder** | [Core/CIPHeader.hpp](Core/CIPHeader.hpp) | IEC 61883-1 header parsing |
| **AM824 Codec** | [Audio/AM824Decoder.hpp](Audio/AM824Decoder.hpp) | IEC 61883-6 sample encoding/decoding |
| **Ring Buffer** | [Encoding/AudioRingBuffer.hpp](Encoding/AudioRingBuffer.hpp) | Lock-free SPSC audio buffer |

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                      ASFWDriver                              │
│  InterruptOccurred → kIsochRx → DispatchAsync(Poll)         │
│  AsyncWatchdog (1kHz) → Poll (timer fallback)               │
└─────────────────────────────────────────────────────────────┘
                              ↕
┌─────────────────────────────────────────────────────────────┐
│                 IsochReceiveContext (IR)                     │
│  • Configure(channel, contextIndex) → Setup OHCI registers   │
│  • Start() → Enable IR interrupt, run DMA context            │
│  • Poll() → Process completed descriptors (interrupt-driven) │
│  • Stop() → Disable interrupt, halt context                  │
└─────────────────────────────────────────────────────────────┘
                              ↕
┌──────────────────┬──────────────────┬──────────────────────┐
│ Memory/          │ Core/            │ Receive/             │
│ DMAMemoryMgr     │ CIPHeader        │ StreamProcessor      │
│ (Dual-slab DMA)  │ (IEC 61883-1)    │ (DBC validation)     │
└──────────────────┴──────────────────┴──────────────────────┘
                              ↕
┌─────────────────────────────────────────────────────────────┐
│                 Audio/AM824Decoder                           │
│  Decode 32-bit AM824 quadlets → 24-bit PCM samples          │
└─────────────────────────────────────────────────────────────┘
```

---

## DMA Memory Architecture

### **Allocation Flow** (See [ASFWDriver.cpp:1037-1105](../ASFWDriver.cpp#L1037-L1105))

```
ASFWDriver::StartIsochReceive(channel)
    ↓
IsochDMAMemoryManager::Create(config)
    ↓
Initialize(hardware)
    ├─ descMgr_.Initialize(hw, 36KB)    ← Descriptor slab
    │   └─ hw.AllocateDMA() → IOBufferMemoryDescriptor
    │
    └─ payloadMgr_.Initialize(hw, 2MB)  ← Payload slab
        ├─ hw.AllocateDMA() → IOBufferMemoryDescriptor
        └─ AlignCursorToIOVA(16384)     ← 16KB page alignment
    ↓
IsochReceiveContext::Create(hw, isochMem)
    ↓
Configure(channel, 0) → SetupRings()
    ├─ AllocateDescriptor(512 × 16B)    ← From descriptor slab
    └─ AllocatePayloadBuffer(512 × 4KB) ← From payload slab
```

### **Memory Layout**

```
┌──────────────────┐              ┌─────────────────────┐
│ Descriptor Slab  │              │   Payload Slab      │
│   ~36KB          │              │    ~2.02MB          │
│                  │              │                     │
│ [Desc 0: 16B]    │──points to──▶│ [Buf 0: 4KB]        │
│ [Desc 1: 16B]    │              │ [Buf 1: 4KB]        │
│ ...              │              │ ...                 │
│ [Desc 511: 16B]  │              │ [Buf 511: 4KB]      │
│                  │              │                     │
│ IOVA: 0xXXXX0    │              │ IOVA: 0xYYYY0000    │
│ (16B aligned)    │              │ (16KB aligned)      │
└──────────────────┘              └─────────────────────┘
```

**Key Design Decisions:**
- **Two independent slabs** prevent fragmentation
- **16KB IOVA alignment** for payloads (macOS DMA requirement)
- **Zero-copy**: Single allocation at startup, no runtime alloc
- **Cache coherency**: `PublishToDevice()` / `FetchFromDevice()` for DMA visibility

---

## Interrupt-Driven Processing

### **Receive Path (IR)**

```
1. OHCI generates kIsochRx interrupt (bit 7) when descriptor completes
2. ASFWDriver::InterruptOccurred reads isoRecvEvent register
3. Clears event bits, dispatches Poll() via workQueue (async)
4. Poll() iterates descriptors, checks xferStatus/resCount
5. StreamProcessor decodes CIP header, validates DBC continuity
6. Re-arms descriptor (reset statusWord), publishes to device
7. [Future] AM824Decoder extracts samples → CoreAudio ring buffer
```

**Hybrid Approach:**
- **Primary**: Interrupt-driven (low latency, ~8000 pkts/sec)
- **Fallback**: 1kHz timer poll (catches missed interrupts)

### **Transmit Path (IT)** 🚧

```
1. CoreAudio → IOOperationHandler writes samples to AudioRingBuffer
2. 1kHz timer → IsochTransmitContext::Poll() (8 packets per tick)
3. PacketAssembler::assembleNext() builds CIP + AM824 packets
4. [Future] Copy to IT DMA descriptors → OHCI transmits
```

**Current Status:** Simulation mode (no DMA yet)

---

## Key Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| **Descriptor ring size** | 512 | ~64ms buffer @ 8kHz |
| **Max packet size** | 4096 bytes | Covers largest AM824 packet |
| **IRQ coalescing** | Every 8th descriptor | `i=3` on desc[7,15,23...] |
| **Timer fallback** | 1 kHz | 1ms tick hybrid backup |
| **DMA descriptor align** | 16 bytes | OHCI requirement |
| **DMA payload align** | 16KB | macOS page alignment |
| **Audio ring buffer** | 4096 frames | ~85ms @ 48kHz (TX path) |

---

## Performance Metrics (Achieved)

| Metric | Before | After |
|--------|--------|-------|
| **Packets/sec** | ~1700 | **~8000** |
| **Drop rate** | ~30% | **0%** |
| **Processing** | Timer-only (500Hz) | Interrupt + Timer hybrid |
| **Latency** | High variance | Consistent low latency |

---

## Protocol Details

### **CIP Header Format (IEC 61883-1)**

```
Quadlet 0: [0]:0 [1-6]:SID [8-15]:DBS [16-17]:FN [18-20]:QPC 
           [21]:SPH [22-23]:rsv [24-31]:DBC
Quadlet 1: [0-1]:fmt [2-7]:FDF [8-15]:rsv [16-31]:SYT
```

**Example Log:**
```
RxStats: Pkts=82918 Data=62189 Empty=20729 Errs=0 Drops=0 | 
         CIP: SID=2 DBS=2 FDF=0x02 SYT=0xB1DA DBC=0x00
```

### **AM824 Format (IEC 61883-6)**

**Label Codes:**
- `0x40`: Multi-bit Linear Audio (MBLA) - 24-bit PCM
- `0x80-0x83`: MIDI conformant data

**Encoding/Decoding:**
```cpp
// Decode (RX)
auto sample = AM824Decoder::DecodeSample(quadlet_be);
// → int32_t (24-bit PCM, sign-extended)

// Encode (TX)
uint32_t quadlet = AM824Encoder::encode(pcm24);
// → 0x40XXXXXX (label 0x40 + 24-bit sample)
```

---

## Component Deep Dive

### **IsochReceiveContext** ([IsochReceiveContext.cpp](IsochReceiveContext.cpp))

**Responsibilities:**
- OHCI IR context management (registers, descriptors)
- Interrupt-driven packet reception
- Descriptor ring management (512 entries)
- Cache coherency (DMA publish/fetch)

**Key Methods:**
- `Configure(channel, contextIndex)`: Set FireWire channel (0-63), OHCI context (0-3)
- `Start()`: Enable IR interrupt, set CommandPtr, run context
- `Poll()`: Process completed descriptors (called from interrupt or timer)
- `Stop()`: Disable interrupt, clear Run bit

**State Machine:** `Stopped → Running → Stopped`

### **IsochDMAMemoryManager** ([Memory/IsochDMAMemoryManager.cpp](Memory/IsochDMAMemoryManager.cpp))

**Responsibilities:**
- Dual-slab DMA allocation (descriptors + payloads)
- IOVA alignment enforcement (16B descriptors, 16KB payloads)
- Cache coherency helpers (`PublishToDevice`, `FetchFromDevice`)

**Critical Methods:**
- `Initialize(hw)`: Allocate both slabs via `hw.AllocateDMA()`
- `AllocateDescriptor(size)`: Allocate from descriptor slab
- `AllocatePayloadBuffer(size)`: Allocate from payload slab (page-aligned)
- `VirtToIOVA(virt)`: Convert virtual address → IOVA for DMA

### **StreamProcessor** ([Receive/StreamProcessor.hpp](Receive/StreamProcessor.hpp))

**Responsibilities:**
- CIP header parsing (IEC 61883-1)
- DBC continuity validation (detect dropped packets)
- Packet classification (DATA vs EMPTY)
- Statistics tracking (packets, errors, discontinuities)

**Metrics:**
- `PacketCount()`, `SamplePacketCount()`, `EmptyPacketCount()`
- `ErrorCount()`, `DiscontinuityCount()`
- `LastDBC()`, `LastSYT()`, `LastCipSID()`

### **PacketAssembler** ([Encoding/PacketAssembler.hpp](Encoding/PacketAssembler.hpp))

**Responsibilities:**
- TX packet assembly (CIP + AM824)
- Orchestrates: `BlockingCadence48k`, `BlockingDbcGenerator`, `AudioRingBuffer`, `AM824Encoder`, `CIPHeaderBuilder`

**Packet Types:**
- **DATA**: 72 bytes (8-byte CIP + 64-byte audio = 8 stereo frames)
- **NO-DATA**: 8 bytes (CIP header only, DBS=0)

**Usage:**
```cpp
PacketAssembler assembler(sid);
assembler.ringBuffer().write(samples, frameCount);
auto pkt = assembler.assembleNext(syt);
// → pkt.data[0..pkt.size-1] ready for transmission
```

---

## Operational Guide

### **Starting IR Reception**

**From UserClient:**
```cpp
driver->StartIsochReceive(channel);
```

**What happens:**
1. Lazy-provision `IsochDMAMemoryManager` (if not already created)
2. Allocate descriptor + payload slabs (~2.06MB total)
3. Create `IsochReceiveContext`
4. Configure for channel N, OHCI context 0
5. Program 512 descriptors (INPUT_LAST, IRQ coalescing)
6. Enable IR interrupt, set Run bit
7. Start processing (interrupt + 1kHz timer fallback)

**Logs to expect:**
```
[Isoch] ✅ Lazily provisioned Isoch Context with Dedicated Memory
[Isoch] ✅ Started IR Context 0 for Channel 0! (Polling enabled via Watchdog)
```

### **Monitoring Performance**

**Every ~1 second:**
```
RxStats: Pkts=8234 Data=6175 Empty=2059 Errs=0 Drops=0 | 
         CIP: SID=2 DBS=2 FDF=0x02 SYT=0xB1DA DBC=0x00
IR: run=1 active=1 dead=0 evt=0x00 lastIdx=123 cap=512
```

**Key indicators:**
- `Pkts`: Total packets received
- `Drops`: Should be **0** (if non-zero, investigate interrupt handling)
- `run=1 active=1`: Context is running and actively receiving
- `dead=1`: **ERROR** - descriptor program issue

### **Stopping IR Reception**

```cpp
driver->StopIsochReceive();
```

**What happens:**
1. Clear Run bit in ContextControl
2. Disable IR interrupt for context 0
3. Wait for Active bit to clear
4. Log final statistics

---

## Troubleshooting

### **Symptom: Pkts=0 (no packets received)**

**Check:**
1. FireWire device is transmitting on correct channel (use FireBug)
2. `IRContextMatch` register set correctly (channel + tag mask)
3. `CommandPtr` points to valid descriptor IOVA
4. Descriptors programmed with correct `dataAddress` IOVAs
5. IR interrupt enabled (`IsoRecvIntMaskSet` bit 0)

**Debug:**
```cpp
ctx.isochReceiveContext->LogHardwareState();
// → Dumps registers + first 8 descriptors
```

### **Symptom: High drop rate (Drops > 0)**

**Causes:**
- Interrupt latency too high (check `DispatchAsync` queue depth)
- Poll() taking too long (check `RecordPollLatency` metrics)
- Descriptor ring too small (increase `kNumDescriptors`)

**Fix:**
- Ensure interrupt-driven path is working (check `kIsochRx` bit in `intEvent`)
- Reduce work in `Poll()` (e.g., disable verbose logging)

### **Symptom: Context dead (dead=1)**

**Causes:**
- Invalid descriptor program (bad `branchWord`, wrong Z value)
- Descriptor IOVA not 16-byte aligned
- Payload IOVA not accessible by DMA

**Fix:**
- Verify `branchWord = nextDescIOVA | Z=1` (Z=0 means STOP!)
- Check `VirtToIOVA()` returns valid IOVA (not 0)
- Ensure `AlignCursorToIOVA(16384)` succeeded

---

## Future Work

### **Receive Path (IR)**
- [ ] AM824 sample extraction → CoreAudio ring buffer
- [ ] Clock synchronization using SYT timestamps
- [ ] Multi-channel support (beyond stereo)

### **Transmit Path (IT)**
- [ ] IT DMA descriptor integration (replace simulation)
- [ ] SYT timestamp calculation for DATA packets
- [ ] Alignment fixes for Device Memory writes (see conversation fecfa8d9)
- [ ] Cadence validation against real devices

### **CoreAudio Integration**
- [ ] Bidirectional audio flow (RX + TX)
- [ ] Sample rate conversion (if needed)
- [ ] Latency compensation

---

## References

**Specifications:**
- IEEE 1394, 1394a/b (FireWire bus)
- IEC 61883-1 (Common Isochronous Packet format)
- IEC 61883-6 (AM824 audio format)
- OHCI 1.1 (Open Host Controller Interface)

**TA Documents:**
- TA 1999008 (Audio Subunit)
- TA 2001007 (Music Subunit)
- TA 2001002 (Stream Formats)

**Legacy Apple Code:**
- `AVCVideoServices-42` (reference implementation)
- `IOFireWireAVC` (AV/C protocol stack)
- `AppleFWOHCI` (OHCI driver)

**Project Docs:**
- [isoch_stack_overview.md](../../.gemini/antigravity/brain/393bcfc1-756a-4283-aa33-202831ebe5d4/isoch_stack_overview.md) (comprehensive technical overview)
