# Bus Management Subsystem

## Overview

The **Bus** subsystem orchestrates IEEE 1394 bus reset handling, topology construction, and bus optimization. It bridges hardware interrupts (OHCI Self-ID packets) to high-level topology snapshots consumed by discovery, implements gap count optimization for bandwidth efficiency, and manages cycle master delegation per IEEE 1394a-2000.

**Purpose**: Transform low-level bus reset chaos into deterministic topology snapshots and optimize bus parameters for performance.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│              ControllerCore (Interrupt Handler)         │
└────────────────────┬────────────────────────────────────┘
                     │ busReset IRQ
           ┌─────────▼─────────┐
           │ BusResetCoordinator│  FSM orchestrator
           │      (9 states)    │
           └─────────┬─────────┘
                     │
      ┌──────────────┼──────────────┬──────────────┐
      │              │              │              │
┌─────▼─────┐  ┌────▼────┐  ┌──────▼───────┐ ┌───▼────────┐
│  SelfID   │  │Topology │  │ BusManager   │ │ GapCount   │
│  Capture  │  │ Manager │  │ (Delegate)   │ │ Optimizer  │
└───────────┘  └─────────┘  └──────────────┘ └────────────┘
      │              │              │              │
┌─────▼─────┐  ┌────▼────┐  ┌──────▼───────┐ ┌───▼────────┐
│  DMA      │  │ Snapshot│  │ PHY Config   │ │ IEEE Table │
│  Buffer   │  │ (Immut) │  │  Packets     │ │   C-2      │
└───────────┘  └─────────┘  └──────────────┘ └────────────┘
```

## Components

### 1. BusResetCoordinator
**FSM orchestrator for deterministic bus reset sequencing**

Implements OHCI §6.1.1, §7.2.3.2, §11 compliance with 9 states:

```cpp
enum class State {
    Idle,               // Normal operation
    Detecting,          // busReset IRQ → mask interrupt
    WaitingSelfID,      // Await selfIDComplete + selfIDComplete2
    QuiescingAT,        // Stop & flush AT contexts
    RestoringConfigROM, // 3-step ROM restoration
    ClearingBusReset,   // Clear busReset bit (preconditions met)
    Rearming,           // Re-enable filters, rearm AT
    Complete,           // Publish metrics, unmask busReset
    Error               // Unrecoverable error
};
```

**FSM Events:**
```cpp
enum class Event {
    IrqBusReset,        // IntEvent.busReset
    IrqSelfIDComplete,  // Bit 16 (first phase)
    IrqSelfIDComplete2, // Bit 15 (sticky, second phase)
    AsyncSynthReset,    // PHY packet in AR/RQ (optional)
    TimeoutGuard,       // Safety timeout
    Unrecoverable,      // Hardware fault
    RegFail             // CSR access failure
};
```

**Critical Sequencing:**
```cpp
void BusResetCoordinator::RunStateMachine() {
    switch (state_) {
    case State::Detecting:
        A_MaskBusReset();          // Narrow IRQ window
        A_StopFlushAT();           // Quiesce transmit
        break;
        
    case State::WaitingSelfID:
        if (G_HaveSelfIDPair()) {  // Both bit 16 + 15 seen
            A_DecodeSelfID();      // Parse DMA buffer
            A_BuildTopology();     // Construct snapshot
            TransitionTo(State::QuiescingAT);
        }
        break;
        
    case State::RestoringConfigROM:
        A_RestoreConfigROM();      // 3-step Linux pattern
        TransitionTo(State::ClearingBusReset);
        break;
        
    case State::ClearingBusReset:
        if (G_ATInactive() && G_ROMImageReady()) {
            A_ClearBusReset();     // Ack busReset bit
            A_SendGlobalResumeIfNeeded();  // Async resume
            TransitionTo(State::Rearming);
        }
        break;
        
    case State::Rearming:
        A_EnableFilters();         // Restore AT filters
        A_RearmAT();               // Restart transmit
        EvaluateRootDelegation();  // Cycle master decision
        TransitionTo(State::Complete);
        break;
        
    case State::Complete:
        A_MetricsLog();            // Publish diagnostics
        A_UnmaskBusReset();        // Re-enable IRQ
        TransitionTo(State::Idle);
        break;
    }
}
```

**Actions (A_*):**
- `A_MaskBusReset()` / `A_UnmaskBusReset()`: IRQ masking
- `A_StopFlushAT()`: Quiesce AT contexts
- `A_DecodeSelfID()`: Parse DMA buffer → `SelfIDCapture::Result`
- `A_BuildTopology()`: Transform Self-ID → `TopologySnapshot`
- `A_RestoreConfigROM()`: Restore header after bus reset (Linux pattern)
- `A_ClearBusReset()`: Ack busReset bit (enables new resets)
- `A_EnableFilters()`: Restore async request/response filters
- `A_RearmAT()`: Re-enable AT transmission
- `A_SendGlobalResumeIfNeeded()`: Async resume after 4s gap
- `A_MetricsLog()`: Publish `BusResetMetrics` to `MetricsSink`

**Guards (G_*):**
- `G_HaveSelfIDPair()`: Both selfIDComplete + selfIDComplete2 seen
- `G_ATInactive()`: AT contexts quiesced (no pending DMA)
- `G_ROMImageReady()`: Config ROM header restored
- `G_NodeIDValid()`: Valid local node ID assigned

---

### 2. SelfIDCapture
**DMA buffer management for OHCI Self-ID packets**

Per OHCI §11, hardware DMAs Self-ID packets to host memory during bus reset:

```cpp
class SelfIDCapture {
public:
    struct Result {
        std::vector<uint32_t> quads;  // Raw quadlets
        std::vector<std::pair<size_t, uint>> sequences;  // (start, count)
        uint32_t generation;
        bool valid;
        bool timedOut;
        bool crcError;
    };
    
    // Lifecycle
    kern_return_t PrepareBuffers(size_t quadCapacity, HardwareInterface& hw);
    kern_return_t Arm(HardwareInterface& hw);  // Write DMA address to register
    void Disarm(HardwareInterface& hw);
    
    // Decode captured Self-ID data
    std::optional<Result> Decode(uint32_t selfIDCountReg, HardwareInterface& hw);
};
```

**DMA Sequence:**
```
1. PrepareBuffers(512 quads)
   ├─ IOBufferMemoryDescriptor::Create(2048 bytes)
   ├─ CreateMapping() → virtual address
   └─ PrepareForDMA() → IOVA address

2. Arm(hw)
   └─ hw.Write(kSelfIDBuffer, segment.address)  // BEFORE linkEnable!

3. Bus reset occurs
   └─ OHCI DMAs Self-ID packets to buffer

4. selfIDComplete IRQ
   └─ Decode(selfIDCountReg)
      ├─ Double-read generation (OHCI §11.3 race detection)
      ├─ Parse sequences (packet 0s)
      ├─ Validate CRC (per IEEE 1394-1995 §8.4.6.2.4)
      └─ Return Result or nullopt
```

**Critical Timing:**
Per OHCI §11.2 and §13.2.5: **Buffer must be armed BEFORE linkEnable** to prevent `UnrecoverableError` from invalid DMA address.

---

### 3. TopologyManager
**Transforms Self-ID data → immutable topology snapshots**

```cpp
class TopologyManager {
public:
    std::optional<TopologySnapshot> UpdateFromSelfID(
        const SelfIDCapture::Result& result,
        uint64_t timestamp,
        uint32_t nodeIDReg);
    
    std::optional<TopologySnapshot> LatestSnapshot() const;
    
    // IRM tracking (Phase 3)
    void MarkNodeAsBadIRM(uint8_t nodeID);
    bool IsNodeBadIRM(uint8_t nodeID) const;
    const std::vector<bool>& GetBadIRMFlags() const;
    
    // Gap count extraction
    static std::vector<uint8_t> ExtractGapCounts(const std::vector<uint32_t>& selfIDs);
};
```

**Topology Construction:**
```cpp
TopologySnapshot TopologyManager::UpdateFromSelfID(...) {
    TopologySnapshot snapshot;
    
    // 1. Parse Self-ID packets into nodes
    for (auto& seq : result.sequences) {
        TopologyNode node = ParseSelfIDSequence(seq);
        snapshot.nodes.push_back(node);
    }
    
    // 2. Analyze topology (IEEE 1394-1995 §8.4)
    snapshot.rootNodeId = FindRootNode(snapshot.nodes);
    snapshot.irmNodeId = FindIRMCandidate(snapshot.nodes, badIRMFlags_);
    snapshot.nodeCount = snapshot.nodes.size();
    snapshot.maxHopsFromRoot = CalculateMaxHops(snapshot.nodes);
    
    // 3. Decode local node ID from OHCI register
    snapshot.localNodeId = (nodeIDReg >> 16) & 0x3F;
    snapshot.busBase16 = nodeIDReg & 0xFFC0;
    snapshot.busNumber = (nodeIDReg >> 16) & 0x3FF;
    
    // 4. Optimize gap count
    snapshot.gapCount = GapCountOptimizer::Calculate(
        snapshot.maxHopsFromRoot,
        std::nullopt  // ping time unavailable on most hardware
    );
    
    return snapshot;
}
```

**TopologyNode Fields:**
```cpp
struct TopologyNode {
    uint8_t nodeId;
    uint8_t portCount;
    uint32_t maxSpeedMbps;      // 100/200/400/800/1600/3200
    bool isIRMCandidate;        // Contender bit set
    bool linkActive;            // Link layer active
    bool initiatedReset;        // Initiated this reset
    bool isRoot;                // Highest node ID with active link
    uint8_t gapCount;           // From Self-ID packet 0
    std::vector<PortState> portStates;  // p0..p15
    std::optional<uint8_t> parentPort;
    std::vector<uint8_t> parentNodeIds;
    std::vector<uint8_t> childNodeIds;
};
```

---

### 4. BusManager
**Cycle master delegation and root node forcing (Apple pattern)**

Implements three critical bus initialization features:

```cpp
class BusManager {
public:
    enum class RootPolicy {
        Auto,       // Defer to OHCI auto-root (highest node ID)
        Delegate,   // Request IRM-capable node become root
        ForceNode   // Force specific node as root
    };
    
    struct Config {
        RootPolicy rootPolicy = RootPolicy::Delegate;
        uint8_t forcedRootNodeID = 0xFF;
        bool forcedGapFlag = false;
        uint8_t forcedGapCount = 0;
        bool enableGapOptimization = false;
    };
    
    // Evaluate topology and generate PHY config packet
    std::optional<PhyConfigCommand> EvaluateTopology(
        const TopologySnapshot& topology,
        const std::vector<uint32_t>& selfIDs,
        uint8_t previousGap = 0xFF);
};
```

**PHY Config Packet Structure:**
```cpp
struct PhyConfigCommand {
    std::optional<uint8_t> gapCount;       // Bits 16-21
    std::optional<uint8_t> forceRootNodeID; // Bits 24-29 (R=1, T=nodeID)
};

// Encoded as IEEE 1394-1995 §8.4.6.3 PHY configuration packet
uint32_t EncodePhyPacket(const PhyConfigCommand& cmd) {
    uint32_t quadlet = 0x00000000;  // PHY packet identifier
    
    if (cmd.gapCount) {
        quadlet |= (*cmd.gapCount & 0x3F) << 16;  // Bits 16-21
    }
    
    if (cmd.forceRootNodeID) {
        quadlet |= (1u << 23);  // R bit (force root)
        quadlet |= (*cmd.forceRootNodeID & 0x3F) << 24;  // T field
    }
    
    return quadlet;
}
```

**Delegation Logic (Apple IOFireWireController.cpp):**
```cpp
std::optional<PhyConfigCommand> BusManager::EvaluateTopology(...) {
    PhyConfigCommand cmd;
    
    // 1. Gap count optimization (if enabled)
    if (config_.enableGapOptimization) {
        uint8_t newGap = GapCountOptimizer::Calculate(
            topology.maxHopsFromRoot,
            std::nullopt  // ping time
        );
        
        auto currentGaps = TopologyManager::ExtractGapCounts(selfIDs);
        if (GapCountOptimizer::ShouldUpdate(currentGaps, newGap, previousGap)) {
            cmd.gapCount = newGap;
        }
    }
    
    // 2. Root delegation (Delegate policy)
    if (config_.rootPolicy == RootPolicy::Delegate) {
        // Find best IRM candidate (highest node ID, not bad IRM)
        auto irm = FindBestIRM(topology);
        
        // Only delegate if IRM != current root AND we haven't retried too many times
        if (irm && *irm != *topology.rootNodeId && delegateRetryCount_ < 5) {
            cmd.forceRootNodeID = *irm;
            delegateRetryCount_++;
        }
    }
    
    return cmd.gapCount || cmd.forceRootNodeID ? std::make_optional(cmd) : std::nullopt;
}
```

**Retry Limit (Linux pattern):**
Per Linux `core-card.c:493`, limit delegation retries to 5 to prevent infinite reset storms if hardware doesn't respond.

---

### 5. GapCountOptimizer
**IEEE 1394a-2000 Table C-2 implementation**

Gap count = mandatory silent period between packets, critical for bandwidth:

```cpp
class GapCountOptimizer {
public:
    // IEEE 1394a-2000 Annex C, Table C-2 (4.5m cables, 144ns PHY delay)
    static constexpr uint8_t GAP_TABLE[26] = {
        63,  // 0 hops (single node)
        5,   // 1 hop
        7,   // 2 hops
        // ... [see implementation]
        63   // 25+ hops
    };
    
    // Calculate from hop count (conservative)
    static uint8_t CalculateFromHops(uint8_t maxHops);
    
    // Calculate from ping time (accurate)
    static uint8_t CalculateFromPing(uint32_t maxPingNs);
    
    // Dual-calculation (Apple pattern)
    static uint8_t Calculate(uint8_t maxHops, std::optional<uint32_t> maxPingNs);
    
    // Update decision logic
    static bool ShouldUpdate(const std::vector<uint8_t>& currentGaps,
                            uint8_t newGap,
                            uint8_t prevGap = 0xFF);
};
```

**Gap Count Impact:**
- Default gap=63: ~40% bandwidth waste (assumes 16-hop daisy chain)
- Optimized gap=5 (1 hop): Frees bandwidth for actual data
- Formula: `bandwidth_overhead = (gap_count × 48.8ns) / 125μs`

**Example:**
```
3-node daisy chain:
  - Default gap=63 → 31μs overhead per packet → ~25% waste
  - Optimized gap=8 → 3.9μs overhead → ~3% waste
  - **8x improvement in usable bandwidth**
```

---

## Self-ID Packet Format

Per IEEE 1394-1995 §8.4.6.2:

### Packet 0 (Mandatory)
```
Bits   Field           Description
31-30  10              Packet identifier (Self-ID)
29-24  NNNNNN          Physical ID (6-bit node address)
23     L               Link active
22     G               Gap count master
21-16  GGGGGG          Gap count (0-63)
15-14  SP              Speed capability (00=S100, 01=S200, 10=S400, 11reserved)
13-11  000             Reserved
10     C               Contender (IRM candidate)
9-8    PP              Power class
7-6    00              Reserved
5-0    IIIIII          Initiated reset flag
```

### Packet 1+ (Extended Port Info)
```
Bits   Field           Description
31-30  11              Packet identifier (more packets)
29-24  NNNNNN          Physical ID (matches packet 0)
23-22  PP              Port p(n+3) status
21-20  PP              Port p(n+2) status
19-18  PP              Port p(n+1) status
17-16  PP              Port p(n) status
... (repeats for ports 0-15)
```

**Port Status Values:**
```
00 = Not connected / not present
01 = Parent (connected to parent node)
10 = Child (connected to child node)
11 = Connected to another port on this node
```

---

## Bus Reset Flow

```
1. Hardware Event: Cable hotplug, PHY reset, or software-initiated
   ↓
2. OHCI generates busReset IRQ (bit 13)
   ↓
3. BusResetCoordinator::OnIrq(kBusReset)
   └─ State: Idle → Detecting
   └─ A_MaskBusReset() (narrow IRQ window)
   └─ A_StopFlushAT() (quiesce transmit)
   ↓
4. OHCI generates selfIDComplete (bit 16)
   └─ OnIrq(kSelfIDComplete)
   └─ selfIDComplete1_ = true
   ↓
5. OHCI generates selfIDComplete2 (bit 15, sticky)
   └─ OnIrq(kSelfIDComplete2)
   └─ selfIDComplete2_ = true
   └─ State: WaitingSelfID → QuiescingAT
   ↓
6. A_DecodeSelfID()
   ├─ SelfIDCapture::Decode(selfIDCountReg)
   ├─ Validate CRC
   ├─ Parse sequences
   └─ Store Result
   ↓
7. A_BuildTopology()
   ├─ TopologyManager::UpdateFromSelfID()
   ├─ Analyze root/IRM/gap
   └─ Publish TopologySnapshot via callback
   ↓
8. A_RestoreConfigROM()
   └─ ConfigROMStager::RestoreHeaderAfterBusReset()
   └─ State: QuiescingAT → ClearingBusReset
   ↓
9. A_ClearBusReset()
   └─ hw.ClearIntEvents(kBusReset)  (ack bit)
   └─ State: ClearingBusReset → Rearming
   ↓
10. A_EnableFilters() + A_RearmAT()
    └─ Restore async filters
    └─ Re-enable AT transmission
    └─ State: Rearming → Complete
    ↓
11. EvaluateRootDelegation()
    ├─ BusManager::EvaluateTopology()
    ├─ Generate PHY config packet (gap + root)
    └─ Stage for deferred transmission (post-FSM)
    ↓
12. A_MetricsLog() + A_UnmaskBusReset()
    └─ Publish BusResetMetrics
    └─ Re-enable busReset IRQ
    └─ State: Complete → Idle
    ↓
13. Discovery::ROMScanner::Begin(gen, topology)
    └─ [Asynchronous device enumeration]
```

## Critical Invariants

### 1. **Self-ID Double Completion** (OHCI §11.3)
Both `selfIDComplete` (bit 16) AND `selfIDComplete2` (bit 15) must be seen before decoding:

```cpp
bool G_HaveSelfIDPair() {
    return selfIDComplete1_ && selfIDComplete2_;
}
```

**Why**: OHCI generates two separate interrupts. Decoding too early reads incomplete DMA buffer → corrupted topology.

### 2. **Generation Validation** (OHCI §11.3)
Double-read generation register to detect racing bus resets:

```cpp
uint32_t gen1 = hw.Read(kSelfIDCount) & 0xFF;
// ... decode buffer ...
uint32_t gen2 = hw.Read(kSelfIDCount) & 0xFF;
if (gen1 != gen2) {
    // Racing bus reset - discard stale data
    return std::nullopt;
}
```

### 3. **AT Quiescence Before busReset Clear** (Linux pattern)
AT contexts must be stopped before clearing `busReset` bit:

```cpp
bool G_ATInactive() {
    return asyncSubsystem_->AreAllATContextsInactive();
}

// Guard prevents premature transition
if (G_ATInactive() && G_ROMImageReady()) {
    A_ClearBusReset();  // Safe to ack
}
```

**Why**: Clearing `busReset` while AT DMA active → descriptor corruption.

### 4. **Config ROM Restoration** (Linux bus_reset_work)
Header quadlet must be restored to DMA buffer after bus reset:

```cpp
void A_RestoreConfigROM() {
    configRomStager_->RestoreHeaderAfterBusReset();
    // Overwrites zeroed header with real value
}
```

**Why**: Per OHCI §5.5.6, `ConfigROMheader` register resets to 0 after bus reset. DMA buffer must be updated.

---

## Performance & Optimization

### Gap Count Impact on Bandwidth
Default vs optimized gap count (3-node chain):

| Gap Count | Overhead/Packet | Bandwidth Waste | Effective Bandwidth |
|-----------|-----------------|-----------------|---------------------|
| 63 (default) | 31μs | ~25% | ~300 Mbps (S400) |
| 8 (optimized) | 3.9μs | ~3% | ~388 Mbps (S400) |

**Formula:** `overhead = gap_count × 48.8ns`

### Bus Reset Latency
Typical timing (from ISR to topology ready):

| Phase | Duration | Notes |
|-------|----------|-------|
| busReset IRQ → selfIDComplete | 10-20ms | OHCI hardware arbitration |
| selfIDComplete → selfIDComplete2 | <1ms | Sticky bit propagation |
| Self-ID decode | ~100μs | Parse 512 quads, CRC validate |
| Topology construction | ~50μs | Analyze root/IRM, build snapshot |
| **Total reset handling** | **10-25ms** | Dominated by hardware |

---

## Design Patterns

### 1. **FSM Orchestration** (Apple pattern)
Complex sequencing via explicit state machine:
- Deterministic transitions
- Guards prevent invalid progressions
- Actions as pure side effects
- Testable in isolation

### 2. **Immutable Snapshots**
`TopologySnapshot` created once, never modified:
- Thread-safe sharing
- No synchronization needed
- Clear ownership boundaries

### 3. **Double-Buffer DMA** (OHCI pattern)
Self-ID buffer prepared before bus reset:
- Prevents `UnrecoverableError`
- Ensures valid IOVA mapping
- Separates lifecycle (alloc → arm → decode)

### 4. **Retry with Backoff** (Linux pattern)
Delegation retry limit prevents reset storms:
- Max 5 attempts (Linux `core-card.c:493`)
- Reset counter on topology change
- Suppress on gap=0 critical error

---

## Dependencies

- **Hardware**: `HardwareInterface` (OHCI registers)
- **Interrupts**: `InterruptManager` (mask synchronization)
- **Async**: `AsyncSubsystem` (AT context quiescence)
- **Config ROM**: `ConfigROMStager` (header restoration)
- **Discovery**: `ROMScanner` (abort in-flight scans)
- **DriverKit**: `IOBufferMemoryDescriptor`, `IODMACommand`, `IODispatchQueue`

---

## Design Goals

1. **OHCI Compliance**: Strict OHCI §6, §7, §11 adherence
2. **Linux Compatibility**: Mirror `firewire/ohci.c` sequencing
3. **Apple Patterns**: FSM-driven, delegation logic, gap optimization
4. **Determinism**: Reproducible reset handling via explicit FSM
5. **Performance**: Gap count optimization for bandwidth efficiency
