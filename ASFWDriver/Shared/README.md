# Shared Transfer Stack

## Overview

The **Shared** subsystem provides the foundational DMA transfer infrastructure used by both Asynchronous (AT/AR) and future Isochronous (IT/IR) contexts in the ASFWDriver. This is a hardware-agnostic layer that abstracts OHCI DMA mechanics behind type-safe, zero-overhead C++ interfaces.

**Why "Shared"?** These components are protocol-agnostic - they work equally well for asynchronous transactions, isochronous streams, or any OHCI DMA-based operation.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   Upper Layers                          │
│         (AsyncSubsystem, Future ISO subsystem)          │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│                Shared Transfer Stack                    │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   Memory     │  │    Rings     │  │  Completion  │  │
│  │ Management   │  │   Buffers    │  │   Tracking   │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│  ┌──────────────┐  ┌──────────────┐                    │
│  │   Context    │  │   Hardware   │                    │
│  │  Management  │  │   Helpers    │                    │
│  └──────────────┘  └──────────────┘                    │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│                OHCI Hardware (DMA)                      │
└─────────────────────────────────────────────────────────┘
```

## Components

### 1. Memory Management (`Memory/`)

#### DMAMemoryManager
**Slab allocator for OHCI-compliant DMA regions**

- **Single allocation model**: One contiguous DMA region, partitioned into sub-regions
- **32-bit IOVA guarantee**: All addresses within OHCI's 32-bit addressing limit
- **16-byte alignment**: Enforces OHCI §7.1.5.1 descriptor alignment
- **Cache coherency**: Automatic handling (uncached mode or cached+sync)

```cpp
DMAMemoryManager dma;
dma.Initialize(hw, 1024 * 1024);  // 1MB slab

// Allocate descriptor region
auto region = dma.AllocateRegion(4096);
// region->virtualBase: CPU-accessible pointer
// region->deviceBase: Device-visible IOVA (32-bit)
// region->size: Aligned size
```

**Key Operations:**
- `Initialize()`: Allocate DMA slab with cache mode selection
- `AllocateRegion()`: Partition slab into aligned sub-regions
- `VirtToIOVA()` / `IOVAToVirt()`: O(1) address translation
- `PublishRange()` / `FetchRange()`: Cache coherency sync

#### PayloadHandle
**RAII handle for DMA payload buffers**

- **Move-only semantics**: Unique ownership (like `std::unique_ptr`)
- **Automatic cleanup**: Clears state on destruction (memory reclaimed at slab teardown)
- **Type-safe access**: `std::span<uint8_t>` for buffer operations
- **Zero overhead**: Inline operations, no runtime cost

```cpp
PayloadHandle handle(dmaMgr, virtAddr, size, physAddr);
auto data = handle.Data();  // std::span<uint8_t>
std::memcpy(data.data(), source, size);
// Automatic cleanup when handle destroyed
```

#### PayloadPolicy
**Modern C++23 ownership abstractions**

Compile-time ownership validation via the `PayloadType` concept:

```cpp
template<typename P>
concept PayloadType = requires(P& p, const P& cp) {
    { p.GetBuffer() } -> std::convertible_to<std::span<uint8_t>>;
    { cp.GetIOVA() } -> std::convertible_to<uint64_t>;
    { cp.GetSize() } -> std::convertible_to<size_t>;
    { p.Release() } -> std::same_as<void>;
    { cp.IsValid() } -> std::convertible_to<bool>;
};
```

**Ownership Wrappers:**

1. **UniquePayload\<T\>**: Unique ownership with RAII
   ```cpp
   UniquePayload<PayloadHandle> payload(std::move(handle));
   transaction->SetPayload(std::move(payload));  // Transfer ownership
   ```

2. **BorrowedPayload\<T\>**: Non-owning reference
   ```cpp
   BorrowedPayload<PayloadHandle> ref(handle);
   auto data = ref->GetBuffer();  // Read-only access
   // No cleanup - we don't own it
   ```

### 2. Ring Buffers (`Rings/`)

#### DescriptorRing
**Circular buffer for OHCI descriptor chains**

- **Atomic head/tail**: Lock-free SPSC ring buffer
- **Hardware integration**: Generates OHCI branch pointer words
- **Z-field tracking**: Manages descriptor count encoding (bits [3:0])
- **Previous-last lookup**: Links new descriptors to existing chains

```cpp
DescriptorRing ring;
ring.Initialize(descriptorSpan);
ring.Finalize(descriptorIOVA);

// Access descriptor
auto* desc = ring.At(ring.Tail());

// Generate branch pointer with Z-field
uint32_t branchWord = ring.CommandPtrWordTo(targetDesc, zBlocks);

// Atomic updates
ring.SetTail(RingHelpers::Advance(ring.Tail(), 1, ring.Capacity()));
```

**Branch Word Format (OHCI §7.1.5.1):**
```
Bits [31:4]: Physical address >> 4 (28 bits)
Bits [3:0]:  Z field (descriptor count - 1)
```

#### BufferRing
**Circular buffer for receive packets**

- **Descriptor + buffer pairing**: Each slot has OHCI descriptor + data buffer
- **Fixed-size buffers**: All buffers same size (e.g., 4KB)
- **DMA-safe recycling**: Proper barriers on buffer reuse
- **Dequeue metadata**: Returns VA, offset, bytes filled, index

```cpp
BufferRing ring;
ring.Initialize(descriptors, buffers, count, bufferSize);
ring.Finalize(descIOVA, bufIOVA);
ring.PublishAllDescriptorsOnce();  // Make available to hardware

// Hardware fills buffer via DMA
auto info = ring.Dequeue();
// info->virtualAddress: Buffer pointer
// info->bytesFilled: Actual data length
// info->descriptorIndex: For recycling

ring.Recycle(info->descriptorIndex);  // Return to hardware
```

#### RingHelpers
**Header-only utilities for ring operations**

```cpp
RingHelpers::IsEmpty(head, tail);
RingHelpers::IsFull(head, tail, capacity);
RingHelpers::Count(head, tail, capacity);
RingHelpers::Advance(index, amount, capacity);
RingHelpers::Available(head, tail, capacity);

// Atomic variants
RingHelpers::IsEmptyAtomic(atomicHead, atomicTail);
RingHelpers::CountAtomic(atomicHead, atomicTail, capacity);
```

### 3. Completion Tracking (`Completion/`)

#### CompletionQueue\<TokenT\>
**SPSC queue for completion tokens using IODataQueueDispatchSource**

- **Type-safe tokens**: Template parameter for completion data
- **SPSC semantics**: Single interrupt producer, single dispatch consumer
- **Lifecycle management**: Explicit activate/deactivate for safe teardown
- **Drop statistics**: Tracks queue overflow events

```cpp
std::unique_ptr<CompletionQueue<TxCompletionToken>> queue;
CompletionQueue<TxCompletionToken>::Create(
    consumerQueue, capacityBytes, dataAvailableAction, queue);

queue->Activate();
queue->SetClientBound();

// From interrupt context
if (!queue->Push(token)) {
    // Queue full
}

// Shutdown
queue->SetClientUnbound();
queue->Deactivate();
```

### 4. Context Management (`Contexts/`)

#### DmaContextManagerBase\<ContextT, RingT, RoleTag, PolicyT\>
**Templated base for DMA context lifecycle**

Provides common operations for AT/AR (and future IT/IR) managers:

```cpp
template<typename ContextT, typename RingT, typename RoleTag, typename PolicyT>
class DmaContextManagerBase {
public:
    using StateEnum = typename PolicyT::State;
    
    StateEnum GetState() const;  // Thread-safe
    void Transition(StateEnum newState, uint32_t txid, const char* why);
    bool PollActiveUs(uint32_t usMax);  // Wait for hardware confirmation
    void IoWriteFence();  // Ensure writes before RUN/WAKE
    void IoReadFence();   // Ensure reads see latest
};
```

**Usage:**
```cpp
class ATManager : public DmaContextManagerBase<
    ATRequestContext, DescriptorRing, ATRequestTag, ATPolicy> {
    // AT-specific operations
};
```

### 5. Hardware Helpers (`Hardware/`)

#### OHCIHelpers
**OHCI constants and endianness conversion**

**OHCI Constants:**
```cpp
constexpr uint32_t kOHCIDmaAddressBits = 32;      // 32-bit addressing
constexpr uint32_t kOHCIBranchAddressBits = 28;   // Bits [31:4] in branch word
```

**Endianness Conversion:**

> **CRITICAL DISTINCTION:**
> - **OHCI Descriptors**: Host byte order (no conversion)
> - **IEEE 1394 Packet Headers**: Big-endian (use these helpers)

```cpp
// Packet header fields (big-endian)
uint32_t header = ToBigEndian32(destOffset);
uint16_t length = FromBigEndian16(packetLength);

// Descriptor fields (host order - NO CONVERSION)
desc.control = controlBits;  // Already in host order
```

## Data Flow

### Transmit (AT) Path

```
1. Allocate payload from DMAMemoryManager
2. Wrap in UniquePayload for automatic cleanup
3. Build OHCI descriptors in DescriptorRing
4. Convert packet headers to big-endian
5. Link descriptors via branch words
6. IoWriteFence() + set hardware RUN bit
7. Hardware DMA fetches & transmits
8. Completion via CompletionQueue
9. UniquePayload auto-releases on destruction
```

### Receive (AR) Path

```
1. BufferRing pre-allocated with buffers
2. PublishAllDescriptorsOnce() to hardware
3. Hardware DMA writes incoming packets
4. Dequeue() returns FilledBufferInfo
5. Parse packet (big-endian → host order)
6. Process data (borrowed reference)
7. Recycle() buffer back to hardware
```

## Design Patterns

### RAII Everywhere
- `PayloadHandle`: Auto-cleanup on destruction
- `UniquePayload`: Move-only unique ownership
- `CompletionQueue`: RAII lifecycle via `Create()`
- `OSSharedPtr`: Reference-counted DriverKit objects

### Lock-Free Rings
- Atomic head/tail pointers
- SPSC semantics (no contention)
- `std::memory_order_acquire/release`
- No locks on hot path

### Compile-Time Safety
- `PayloadType` concept enforces interface
- `static_assert` for OHCI constraints
- Template constraints prevent misuse
- Errors caught at compile time

### Slab Allocation
- Single DMA allocation at init
- Partition on demand (no fragmentation)
- No individual free operations
- Memory reclaimed at slab destruction

## Thread Safety

| Component | Thread Safety | Notes |
|-----------|--------------|-------|
| `DMAMemoryManager` | Initialize: unsafe<br>Translate: safe | Sequential init, read-only after |
| `DescriptorRing` | Head/tail: atomic<br>Descriptors: single writer | SPSC semantics |
| `BufferRing` | Head: atomic<br>Buffers: single writer | SPSC semantics |
| `CompletionQueue` | Producer: IRQ context<br>Consumer: dispatch queue | SPSC via IODataQueue |
| `DmaContextManagerBase` | State: locked<br>Polling: safe | `IOLock` protects transitions |

## Performance

- **Memory**: O(1) allocation, zero fragmentation, cache-line aligned
- **Address translation**: O(1) pointer arithmetic (no table lookups)
- **Ring operations**: Lock-free atomics (acquire/release ordering)
- **Completion**: Single IODataQueue enqueue (no memory allocation)

## Usage Example

```cpp
// Initialize DMA manager
DMAMemoryManager dma;
dma.Initialize(hw, 1024 * 1024);

// Allocate descriptor ring
auto descRegion = dma.AllocateRegion(4096);
auto descSpan = std::span<HW::OHCIDescriptor>(
    reinterpret_cast<HW::OHCIDescriptor*>(descRegion->virtualBase),
    descRegion->size / sizeof(HW::OHCIDescriptor));

DescriptorRing ring;
ring.Initialize(descSpan);
ring.Finalize(descRegion->deviceBase);

// Allocate payload
auto payloadRegion = dma.AllocateRegion(1024);
PayloadHandle handle(&dma, payloadRegion->virtualBase, 
                     payloadRegion->size, payloadRegion->deviceBase);
UniquePayload<PayloadHandle> payload(std::move(handle));

// Write data
auto data = payload->Data();
std::memcpy(data.data(), sourceBuffer, sourceSize);

// Build descriptor
auto* desc = ring.At(ring.Tail());
desc->dataAddress = payload->PhysicalAddress();
desc->control = HW::BuildControlWord(sourceSize);

// Submit to hardware
ring.SetTail(RingHelpers::Advance(ring.Tail(), 1, ring.Capacity()));
```

## Isochronous Readiness

This stack is **80% ready** for isochronous support:

**Ready:**
- ✅ DMA memory management (same requirements)
- ✅ Ring buffers (continuous streaming pattern already present)
- ✅ Cache coherency (works for IT/IR buffers)
- ✅ Context lifecycle (template accepts `ITContext`/`IRContext`)

**Needs addition:**
- ⚠️ Cycle timing (125µs synchronization)
- ⚠️ Channel allocation (64 channel bitmap)
- ⚠️ Bandwidth management (speed-dependent calculations)
- ⚠️ IT/IR descriptor formats (different fields, same structure)

## Dependencies

- **DriverKit**: `IOBufferMemoryDescriptor`, `IODMACommand`, `IOLock`, `IODataQueueDispatchSource`
- **C++23**: Concepts, spans, atomics, move semantics, `std::optional`
- **OHCI 1.1**: Descriptor formats, addressing constraints, cache coherency
- **IEEE 1394**: Packet header endianness (big-endian)

## Design Philosophy

1. **Zero overhead**: Inline operations, constexpr evaluation, no runtime cost
2. **Type safety**: Concepts enforce interfaces at compile time
3. **Explicit ownership**: Clear semantics prevent lifetime bugs
4. **Hardware abstraction**: OHCI details hidden behind clean APIs
5. **Fail-fast**: Validate constraints early (compile time > init time > runtime)
