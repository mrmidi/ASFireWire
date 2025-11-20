# Config ROM Subsystem

## Overview

The **ConfigROM** subsystem handles IEEE 1394 Configuration ROM operations for both local (host) and remote (device) nodes. It provides bidirectional ROM functionality: building the driver's own Config ROM for bus presentation, and scanning/parsing remote device ROMs for discovery.

**Purpose**: Bridge between IEEE 1394 discovery protocol and device enumeration, enabling automatic device identification and capability detection.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Discovery Layer                      │
│           (DeviceManager, TopologyManager)              │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│              ConfigROM Subsystem                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  ROMScanner  │  │  ROMReader   │  │ ConfigROM    │  │
│  │   (Scan)     │  │   (Fetch)    │  │   Store      │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│  ┌──────────────┐  ┌──────────────┐                    │
│  │ ConfigROM    │  │ ConfigROM    │                    │
│  │  Builder     │  │   Stager     │                    │
│  │  (Local)     │  │ (Hardware)   │                    │
│  └──────────────┘  └──────────────┘                    │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│         AsyncSubsystem / HardwareInterface              │
│          (Async transactions, DMA buffers)              │
└─────────────────────────────────────────────────────────┘
```

## Components

### 1. Remote ROM Discovery (Read Path)

#### ROMScanner
**FSM-driven orchestrator for multi-node scanning with bounded concurrency**

Manages the complete scan lifecycle for all nodes in a topology generation:

```cpp
ROMScanner scanner(bus, speedPolicy, onComplete, dispatchQueue);

// Begin scan for generation 4
scanner.Begin(gen, topology, localNodeId);

// Wait for completion (via callback)
// onComplete(gen) → all nodes processed

// Retrieve discovered ROMs
std::vector<ConfigROM> roms = scanner.DrainReady(gen);
```

**Key Features:**
- **Bounded concurrency**: Configurable max in-flight operations (default: 4)
- **Speed fallback**: S400 → S200 → S100 on errors (Apple pattern)
- **Retry logic**: Configurable attempts per speed tier
- **IRM verification**: CAS test on `CHANNELS_AVAILABLE` register
- **Generation safety**: Aborts stale operations on bus reset

**FSM States:**
```
Idle → ReadingBIB → VerifyingIRM_Read → VerifyingIRM_Lock → 
  ReadingRootDir → Complete/Failed
```

**IRM Verification (Phase 3):**
Per IEEE 1394a-2000 §8.3.2.3.10, verifies IRM capability via:
1. Read `CHANNELS_AVAILABLE_HI` (0xFFFFF0000404)
2. CompareSwap test on same register
3. Mark node as bad IRM if either fails

#### ROMReader
**Low-level async transaction wrapper for Config ROM reads**

Abstracts `IFireWireBus` for ROM-specific operations:

```cpp
ROMReader reader(bus, dispatchQueue);

// Read Bus Info Block (5 quadlets @ 0xFFFFF0000400)
reader.ReadBIB(nodeId, gen, speed, [](const ReadResult& result) {
    if (result.success) {
        // Parse BIB: GUID, max_rec, vendor_id, etc.
    }
});

// Read root directory quadlets
reader.ReadRootDirQuadlets(nodeId, gen, speed, offset, count, callback);
```

**Features:**
- **Quadlet-only**: Block reads problematic for Config ROM (Apple avoids)
- **S100 enforcement**: Always uses S100 for compatibility
- **Async dispatch**: Optional dispatch queue for deferred execution
- **Generation tracking**: Validates generation consistency

#### ConfigROMStore
**Persistent storage and caching for discovered device ROMs**

Provides CRUD operations and efficient lookup:

```cpp
ConfigROMStore store;

// Store discovered ROM
store.Store(nodeId, gen, romData);

// Lookup by node ID
if (auto rom = store.Lookup(nodeId, gen)) {
    // Parse capabilities
}

// Invalidate on bus reset
store.InvalidateGeneration(oldGen);
```

**Capabilities:**
- Thread-safe concurrent access (`IOLock`)
- Generation-based invalidation
- GUID-based deduplication
- Persistent across resets (tracks generation changes)

### 2. Local ROM Generation (Write Path)

#### ConfigROMBuilder
**Builds 1KB Config ROM image in big-endian format per IEEE 1394-1995 §8.3.2**

Staged API for flexible ROM construction:

```cpp
ConfigROMBuilder builder;

// Stage 1: Begin with mandatory fields
builder.Begin(busOptions, guid, nodeCapabilities);

// Stage 2: Add directory entries
builder.AddImmediateEntry(0x03, vendorID);  // Vendor ID
auto leaf = builder.AddTextLeaf(0x81, "ASFireWire Driver");  // Vendor name

// Stage 3: Finalize (computes CRCs, generates header)
builder.Finalize();

// Get ROM images
auto beImage = builder.ImageBE();      // For wire transmission (big-endian)
auto nativeImage = builder.ImageNative();  // For DMA buffer (host order)
```

**Structure (IEEE 1394-1995 §8.3.2):**
```
┌────────────────────────────────────┐
│ Bus Info Block (5 quadlets)        │  0x0000-0x0013
│  - Header (info_length, CRC)       │
│  - Bus Info (bus_name, capabilities)
│  - GUID (high 32 bits)             │
│  - GUID (low 32 bits)              │
│  - Reserved                        │
├────────────────────────────────────┤
│ Root Directory                     │  0x0014+
│  - Length + CRC                    │
│  - Module Vendor ID (immediate)    │
│  - Node Capabilities (immediate)   │
│  - Text Descriptor Leaf (offset)   │
│  - ...                             │
├────────────────────────────────────┤
│ Text Descriptor Leaves             │
│  - Vendor Name ("ASFireWire")      │
│  - Model Name                      │
└────────────────────────────────────┘
```

**CRC-16 (IEEE 1212):**
- Polynomial: `0x1021` (CRC-16-CCITT)
- Initial value: `0x0000`
- Computed over `[length, data...]` for each block

**Key Methods:**
- `Build()`: Legacy single-shot builder
- `Begin()`, `AddImmediateEntry()`, `AddTextLeaf()`, `Finalize()`: Staged API
- `UpdateGeneration()`: Increment generation on bus reset
- `ImageBE()`: Big-endian image (for transmission)
- `ImageNative()`: Host-order image (for OHCI DMA buffer)

#### ConfigROMStager
**Hardware-aware ROM staging and shadow register management per OHCI §5.5**

Bridges `ConfigROMBuilder` output to OHCI registers:

```cpp
ConfigROMStager stager(hw);

// Stage ROM for next bus reset
kern_return_t kr = stager.StageNewROM(builder);

// Shadow registers will activate on bus reset

// Restore after bus reset (OHCI §5.5.6)
stager.RestoreAfterBusReset();  // Rewrite ConfigROMheader
```

**OHCI Shadow Registers (§5.5.6):**
1. **ConfigROMheader** (0x18): Written LAST to activate shadow
2. **BusOptions** (0x1C): Bus capabilities
3. **GUIDHi** (0x24) / **GUIDLo** (0x28): Write-once GUID
4. **ConfigROMmap** (0x34): Physical address of full ROM buffer

**Shadow Activation Sequence:**
```cpp
// 1. Allocate DMA buffer
IOBufferMemoryDescriptor* romBuffer = ...;

// 2. Write full ROM to buffer (host byte order)
memcpy(bufferVA, builder.ImageNative().data(), size);

// 3. Program shadow registers
hw.Write32(kOHCIRegBusOptions, busOptions);
hw.Write32(kOHCIRegGUIDHi, guidHi);
hw.Write32(kOHCIRegGUIDLo, guidLo);
hw.Write32(kOHCIRegConfigROMmap, bufferPA);

// 4. Activate shadow (triggers on next bus reset)
hw.Write32(kOHCIRegConfigROMheader, headerQuad);
```

**Post-Reset Restoration:**
Per OHCI §5.5.6, `ConfigROMheader` resets to zero after bus reset. Driver must:
1. Rewrite header quadlet to buffer (DMA view)
2. Rewrite `BusOptions` register
3. Rewrite `ConfigROMheader` register

## Data Flow

### Remote Discovery Flow

```
1. TopologyManager detects bus reset → new generation
   ↓
2. ROMScanner::Begin(gen, topology, localNodeId)
   ↓
3. For each remote node:
   ├─ ReadBIB (5 quadlets @ 0x400)
   │  └─ Extract: vendor_id, max_rec, GUID
   ├─ (If IRM) Verify with CAS test
   └─ ReadRootDirQuadlets (bounded scan)
      └─ Parse: unit directories, text leaves
   ↓
4. ROMScanner::CheckAndNotifyCompletion()
   → onScanComplete(gen)
   ↓
5. DeviceManager::DrainReady(gen)
   → Enumerate devices from ROMs
```

### Local ROM Staging Flow

```
1. ControllerCore::Initialize()
   ↓
2. ConfigROMBuilder::Build(busOptions, guid, caps, vendor)
   ↓
3. ConfigROMStager::StageNewROM(builder)
   ├─ Allocate DMA buffer (1KB)
   ├─ Copy ROM to buffer (host byte order)
   ├─ Program OHCI shadow registers
   └─ Write ConfigROMheader (activates on reset)
   ↓
4. Bus reset occurs
   ├─ OHCI auto-loads shadow → active Config ROM
   └─ ConfigROMheader zeroed
   ↓
5. ControllerCore::OnBusReset()
   └─ ConfigROMStager::RestoreAfterBusReset()
      ├─ Rewrite buffer header (DMA)
      ├─ Rewrite BusOptions register
      └─ Rewrite ConfigROMheader register
```

## Types and Structures

### ConfigROM (DiscoveryTypes.hpp)
```cpp
struct ConfigROM {
    uint8_t nodeId;
    Generation generation;
    uint64_t guid;
    uint32_t vendorId;
    uint32_t modelId;
    uint8_t maxRec;
    FwSpeed maxSpeed;
    std::vector<uint32_t> bibQuadlets;      // Bus Info Block
    std::vector<uint32_t> rootDirQuadlets;  // Root directory
};
```

### ROMScannerParams
```cpp
struct ROMScannerParams {
    uint8_t maxConcurrentOps = 4;    // Bounded concurrency
    uint8_t maxRetriesPerSpeed = 2;  // Retry before fallback
    uint32_t timeoutMs = 200;        // Per-operation timeout
};
```

### LeafHandle (ConfigROMBuilder)
```cpp
struct LeafHandle {
    uint32_t leafIndex;     // Index in ROM buffer
    uint32_t quadletOffset; // Offset for directory entry
};
```

## IEEE 1394 Specifications

### Config ROM Address Map (IEEE 1394-1995 §8.3.2)
```
0xFFFFF0000000 - 0xFFFFF00003FF: Initial register space
0xFFFFF0000400 - 0xFFFFF00007FF: Config ROM (1KB)
0xFFFFF0000800+: Unit directories (optional)
```

### Bus Info Block Format
```
Quadlet 0: [info_length:8][crc:16][generation:8]
Quadlet 1: [bus_name:24 = 0x31333934]['1']['3']['9']['4'] | [irmc:1][cmc:1][isc:1][bmc:1][pmc:1][cyc_clk_acc:8][max_rec:4]
Quadlet 2: GUID high 32 bits
Quadlet 3: GUID low 32 bits  
Quadlet 4: Reserved
```

### Root Directory Keys (IEEE 1212)
```
0x03: Vendor ID (immediate)
0x0C: Node capabilities (immediate)
0x17: Model ID (immediate)
0x81: Textual descriptor (offset to leaf)
0xD1: Unit directory (offset)
```

## Design Patterns

### FSM-Driven Scanning (Apple Pattern)
Linux's `fw_node_read()` blocks on completion. Apple uses FSM:
```cpp
// Apple: IOFWNodeScan - state machine
enum { kScanning, kComplete, kFailed };
```

Our `ROMScanner` improves with:
- Bounded concurrency (Linux serializes)
- Speed fallback (Apple: S400 → S200 → S100)
- Immediate completion callback (vs polling)

### Quadlet-Only Reads
**Why no block reads?** Per Apple observation:
- Many devices' Config ROM block implementation is buggy
- Quadlet reads are universally reliable
- Performance difference negligible (ROM reads are infrequent)

### Shadow Register Management
OHCI §5.5.6 shadow pattern:
1. Stage all shadow registers
2. Write `ConfigROMheader` LAST to activate
3. Restore `ConfigROMheader` after each bus reset

## Thread Safety

| Component | Safety | Notes |
|-----------|--------|-------|
| `ROMScanner` | Single-threaded | Must be called from same dispatch queue |
| `ROMReader` | Async-safe | Dispatch queue optional |
| `ConfigROMStore` | Thread-safe | `IOLock` protected |
| `ConfigROMBuilder` | Unsafe | Build on single thread |
| `ConfigROMStager` | Unsafe | Called from ControllerCore only |

## Performance

- **Bounded concurrency**: Max 4 parallel ROM reads (configurable)
- **Speed fallback**: Automatic downgrade on errors (no manual intervention)
- **Generation tracking**: Abort stale operations immediately on bus reset
- **CRC computation**: Optimized table-free CRC-16 (inline shifts)
- **Memory**: Fixed 1KB buffer per local ROM, dynamic per remote

## Usage Examples

### Building Local Config ROM
```cpp
ConfigROMBuilder builder;
builder.Begin(0x0000BB03, 0x001122334455667788, 0x0083C0E0);
builder.AddImmediateEntry(0x03, 0x00AABBCC);  // Vendor ID
builder.AddTextLeaf(0x81, "My FireWire Device");
builder.Finalize();

ConfigROMStager stager(hw);
stager.StageNewROM(builder);
```

### Scanning Remote Devices
```cpp
ROMScanner scanner(bus, speedPolicy, [](Generation gen) {
    ASFW_LOG(Discovery, "Scan complete for gen %u", gen);
}, dispatchQueue);

scanner.Begin(gen, topology, localNodeId);
// ... wait for callback ...

auto roms = scanner.DrainReady(gen);
for (const auto& rom : roms) {
    if (rom.vendorId == 0x00A0B0) {
        // Found device of interest
    }
}
```

### Manual ROM Read (Debug)
```cpp
ROMReader reader(bus, nullptr);
reader.ReadBIB(nodeId, gen, FwSpeed::S100, [](const ReadResult& r) {
    if (r.success) {
        uint64_t guid = ExtractGUID(r.data);
        ASFW_LOG(ConfigROM, "GUID: %016llx", guid);
    }
});
```

## Dependencies

- **AsyncSubsystem**: Async read transactions (`IFireWireBus`)
- **HardwareInterface**: OHCI register access, DMA allocation
- **TopologyManager**: Topology snapshots, IRM reporting
- **DriverKit**: `IOBufferMemoryDescriptor`, `IODispatchQueue`, `IOLock`

## Design Goals

1. **Reliability**: Quadlet-only reads, retry with speed fallback
2. **Efficiency**: Bounded concurrency, generation-based abort
3. **Compliance**: Full IEEE 1394/IEEE 1212/OHCI spec adherence
4. **Apple Compatibility**: FSM pattern, shadow register handling
5. **Debuggability**: Manual read API, comprehensive logging
