# Controller Core Subsystem

## Overview

The **Controller** subsystem is the **central orchestrator** that wires together all driver components: hardware initialization, interrupt routing, bus reset sequencing, topology management, and discovery coordination. It owns the driver's lifecycle from PCI attachment through runtime operation to teardown.

**Purpose**: Provide a single authority for OHCI controller initialization, state management, and event routing to specialized subsystems.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     IOService (DriverKit)                   │
│                     ASFWDriver::Start()                     │
└────────────────────┬────────────────────────────────────────┘
                     │
           ┌─────────▼─────────┐
           │  ControllerCore   │  Central orchestrator
           │  (Dependencies)   │  
           └─────────┬─────────┘
                     │
      ┌──────────────┼──────────────┬──────────────┐
      │              │              │              │
┌─────▼─────┐  ┌────▼────┐  ┌──────▼───────┐ ┌───▼───────┐
│ Hardware  │  │ BusReset│  │  Discovery   │ │  Async    │
│ Interface │  │  FSM    │  │  Integration │ │ Subsystem │
└───────────┘  └─────────┘  └──────────────┘ └───────────┘
      │              │              │              │
┌─────▼─────┐  ┌────▼────┐  ┌──────▼───────┐ ┌───▼───────┐
│  OHCI     │  │Topology │  │  ROMScanner  │ │  DMA Ring │
│ Registers │  │ Manager │  │DeviceManager │ │  Engines  │
└───────────┘  └─────────┘  └──────────────┘ └───────────┘
```

## Components

### 1. ControllerCore
**Central orchestrator with dependency injection pattern**

Manages the complete lifecycle via dependencies:

```cpp
struct Dependencies {
    // Hardware layer
    std::shared_ptr<HardwareInterface> hardware;
    std::shared_ptr<InterruptManager> interrupts;
    
    // Bus management
    std::shared_ptr<BusResetCoordinator> busReset;
    std::shared_ptr<TopologyManager> topology;
    std::shared_ptr<SelfIDCapture> selfId;
    std::shared_ptr<BusManager> busManager;
    
    // Discovery
    std::shared_ptr<ROMScanner> romScanner;
    std::shared_ptr<DeviceManager> deviceManager;
    std::shared_ptr<ConfigROMStore> romStore;
    std::shared_ptr<SpeedPolicy> speedPolicy;
    
    // Async transactions
    std::shared_ptr<AsyncSubsystem> asyncSubsystem;
    
    // Config ROM
    std::shared_ptr<ConfigROMBuilder> configRom;
    std::shared_ptr<ConfigROMStager> configRomStager;
    
    // State & scheduling
    std::shared_ptr<ControllerStateMachine> stateMachine;
    std::shared_ptr<Scheduler> scheduler;
    std::shared_ptr<MetricsSink> metrics;
};
```

**Key Methods:**
- `Start(IOService*)`: Initialize hardware, arm bus, enable interrupts
- `Stop()`: Shutdown sequence (interrupts → async → hardware)
- `HandleInterrupt()`: Route OHCI interrupts to subsystems
- `Bus()` / `DMA()`: Interface facades for stable API

### 2. Controller Initialization Sequence

Per Linux `ohci_enable()` and OHCI §5.7 compliance:

```
1. Software Reset (PerformSoftReset)
   └─ Set softReset bit, poll for clear (500ms timeout)

2. Link Power Status (InitialiseHardware)
   ├─ Set LPS + postedWriteEnable
   ├─ Poll LPS with retry (3× 50ms, handles flaky PHYs)
   └─ 50ms settling delay (TI TSB82AA2 quirk)

3. PHY Configuration (IEEE 1394a-2000 §4.3.4.1)
   ├─ Open PHY gate (programPhyEnable=1)
   ├─ Probe PHY (read reg1, retry with LPS toggle if needed)
   ├─ Configure reg4: link_on + contender
   ├─ Set/clear aPhyEnhanceEnable (match PHY capability)
   └─ Close gate (programPhyEnable=0) ← CRITICAL per OHCI §5.7.2

4. Config ROM Staging (OHCI §5.5.6)
   ├─ Allocate 1KB DMA buffer
   ├─ Write ROM in NATIVE byte order
   ├─ Zero header quadlet (Linux pattern)
   ├─ Program shadow registers (BusOptions, GUID, ConfigROMmap)
   └─ Write ConfigROMheader LAST (activates on bus reset)

5. Self-ID Buffer (OHCI §11.2)
   ├─ Allocate 2KB DMA buffer (512 quadlets for 64 nodes)
   ├─ Arm buffer BEFORE linkEnable
   └─ Prevents UnrecoverableError from invalid DMA address

6. Enable Link (EnableInterruptsAndStartBus)
   ├─ Seed IntMask (baseline + masterIntEnable)
   ├─ Set linkEnable + BIBimageValid atomically
   ├─ Force PHY bus reset (shadow activation)
   └─ Arm AR contexts (receive enabled, transmit deferred)
```

### 3. Interrupt Routing

**HandleInterrupt()** dispatches OHCI events to subsystems:

```cpp
void HandleInterrupt(const InterruptSnapshot& snapshot) {
    uint32_t events = snapshot.intEvent & enabledMask;
    
    // Critical errors (OHCI §13)
    if (events & kUnrecoverableError) {
        DiagnoseUnrecoverableError();  // regAccessFail, postedWriteErr
    }
    
    // Bus reset (delegated to BusResetCoordinator FSM)
    if (events & kBusReset) {
        busReset->OnIrq(events, timestamp);
        // FSM handles: AT flush, Self-ID decode, topology build, ROM restore
    }
    
    // Async DMA completions
    if (events & kReqTxComplete) asyncSubsystem->OnTxInterrupt();
    if (events & kRQPkt) asyncSubsystem->OnRxInterrupt(ARRequest);
    if (events & kRSPkt) asyncSubsystem->OnRxInterrupt(ARResponse);
    
    // Cycle timing
    if (events & kCycleTooLong) { /* ISO overrun */ }
    if (events & kCycle64Seconds) { /* 64s rollover */ }
}
```

### 4. ControllerStateMachine
**FSM for lifecycle tracking (observable state)**

```cpp
enum class ControllerState {
    kCreated,      // Initial construction
    kStarting,     // Start() in progress
    kRunning,      // Fully operational
    kQuiescing,    // Stop() in progress
    kStopped,      // Cleanly shutdown
    kFailed        // Unrecoverable error
};
```

Transitions logged via `TransitionTo(state, reason, timestamp)`.

### 5. ControllerConfig
**Static configuration (vendor name, capabilities)**

```cpp
struct ControllerConfig {
    std::string vendorName = "ASFireWire";
    uint32_t nodeCapabilities = 0x00000001;  // Basic node (no ISO)
    uint64_t guid = 0;  // Auto-generated if 0
};
```

### 6. Types & Snapshots

#### InterruptSnapshot
**Captured in ISR before routing to work queue:**
```cpp
struct InterruptSnapshot {
    uint32_t intEvent;       // Raw OHCI IntEvent register
    uint32_t intMask;        // Current IntMask (for filtering)
    uint32_t isoXmitEvent;   // ISO transmit context events
    uint32_t isoRecvEvent;   // ISO receive context events
    uint64_t timestamp;      // mach_absolute_time()
};
```

#### TopologySnapshot
**Immutable topology for consumers (UI, discovery):**
```cpp
struct TopologySnapshot {
    uint32_t generation;
    std::vector<TopologyNode> nodes;
    
    // Analysis results (IEEE 1394-1995 §8.4)
    std::optional<uint8_t> rootNodeId;
    std::optional<uint8_t> irmNodeId;
    std::optional<uint8_t> localNodeId;
    uint8_t gapCount;
    uint8_t nodeCount;
    uint8_t maxHopsFromRoot;
    uint16_t busBase16;
    
    // Raw Self-ID data
    SelfIDMetrics selfIDData;
    std::vector<std::string> warnings;
};
```

#### SharedStatusBlock
**256-byte status block for GUI (via shared memory):**
```cpp
struct SharedStatusBlock {
    uint32_t version;
    uint64_t sequence;              // Increments on update
    uint64_t updateTimestamp;       // mach_absolute_time()
    uint32_t reason;                // SharedStatusReason enum
    
    char controllerStateName[32];   // Human-readable state
    uint32_t flags;                 // isIRM, isCycleMaster, linkActive
    
    uint32_t busGeneration;
    uint32_t nodeCount;
    uint32_t localNodeID;
    uint32_t rootNodeID;
    uint32_t irmNodeID;
    
    uint64_t busResetCount;
    uint64_t asyncPending;
    uint64_t asyncTimeouts;
    
    uint8_t reserved[104];          // Future expansion
};
static_assert(sizeof(SharedStatusBlock) == 256);
```

## OHCI Initialization Patterns

### Linux Compliance (firewire/ohci.c)
Our initialization sequence mirrors Linux's `ohci_enable()`:

| Step | Linux (ohci.c) | Our Code | Notes |
|------|----------------|----------|-------|
| Soft reset | Line 2415-2426 | `PerformSoftReset()` | 500ms timeout, poll for clear |
| LPS enable | Line 2428-2445 | `SetHCControlBits(kLPS)` | 3× retry for flaky PHYs |
| TI quirk | Line 2437-2440 | 50ms settle | TSB82AA2 needs delay |
| PHY probe | Line 2372-2389 | `ReadPhyRegister(1)` | Gate+settle+probe sequence |
| PHY reg4 | Line 2511 | link_on + contender | IEEE 1394a-2000 §4.3.4.1 |
| programPhyEnable clear | Line 2387 | `ClearHCControlBits()` | **CRITICAL** per OHCI §5.7.2 |
| ConfigROM | Line 2551-2557 | `StageConfigROM()` | Zero header, shadow activation |
| Self-ID buffer | Line 2471-2473 | `PrepareBuffers()` + `Arm()` | Before linkEnable |
| IntMask | Line 2583-2586 | Seed with baseline | masterIntEnable last |
| linkEnable | Line 2572-2574 | Atomic with BIBimageValid | Triggers auto bus reset |

### Apple Patterns (IOFireWireController.cpp)

| Pattern | Apple Implementation | Our Implementation |
|---------|---------------------|-------------------|
| FSM-driven bus reset | State machine orchestration | `BusResetCoordinator` FSM |
| Immediate completion callbacks | Pull-based ready check | ROMScanner → `OnDiscoveryScanComplete()` |
| IRM verification | CAS test on CHANNELS_AVAILABLE | Phase 3 in ROMScanner |
| Bad IRM tracking | `fIRMisBad` flag | `TopologyManager::MarkNodeAsBadIRM()` |
| Generation gating | txnManager gen validation | `TransactionManager` per-gen tracking |

### Critical Fixes & Quirks

#### 1. **programPhyEnable Gate** (OHCI §5.7.2)
```cpp
// WRONG (causes UnrecoverableError on some hardware):
hw.SetHCControlBits(kProgramPhyEnable);
// ... configure PHY ...
// (never cleared - leaves hardware in config mode!)

// CORRECT:
hw.SetHCControlBits(kProgramPhyEnable);  // Open gate
// ... configure PHY ...
hw.ClearHCControlBits(kProgramPhyEnable); // MUST close gate!
```

**Why**: OHCI §5.7.2 requires clearing `programPhyEnable` after PHY/Link configured. Leaving it set causes undefined behavior.

#### 2. **TI TSB82AA2 LPS Quirk** (Linux line 2437-2440)
```cpp
// LPS may signal early but PHY not ready
IOSleep(50);  // Post-LPS settling
phyId = hw.ReadPhyRegister(1);
if (!phyId) {
    // Retry with LPS toggle
    hw.ClearHCControlBits(kLPS);
    IODelay(5000);
    hw.SetHCControlBits(kLPS);
    IOSleep(50);
}
```

#### 3. **Config ROM Header Zeroing** (Linux line 2551)
```cpp
savedHeader_ = buffer[0];  // Save for post-reset restore
buffer[0] = 0;  // Zero header (marks "not ready")
// Write ConfigROMheader register with real value
// DMA buffer header restored in RestoreHeaderAfterBusReset()
```

**Why**: Prevents early ROM reads during shadow activation.

#### 4. **Self-ID Buffer Timing** (OHCI §11.2, §13.2.5)
```cpp
// WRONG: Arm after linkEnable
hw.SetHCControlBits(kLinkEnable);  // Triggers bus reset
selfId->Arm(hw);  // TOO LATE - UnrecoverableError!

// CORRECT: Arm BEFORE linkEnable
selfId->PrepareBuffers(512, hw);  // Allocate DMA
selfId->Arm(hw);  // Write address to register
hw.SetHCControlBits(kLinkEnable);  // NOW safe
```

**Why**: Bus reset triggers Self-ID capture. Invalid DMA address → `UnrecoverableError` + `postedWriteErr`.

## Discovery Integration

ControllerCore wires topology events to discovery:

```cpp
// 1. Topology builds after Self-ID decode
busReset->BindCallbacks([this](const TopologySnapshot& snap) {
    OnTopologyReady(snap);  // Trigger ROMScanner::Begin()
});

// 2. ROMScanner completes asynchronously
romScanner->SetCompletionCallback([this](Generation gen) {
    OnDiscoveryScanComplete(gen);  // Pull ROMs, enumerate devices
});

// 3. DeviceManager processes discovered ROMs
void OnDiscoveryScanComplete(Generation gen) {
    auto roms = romScanner->DrainReady(gen);
    deviceManager->ProcessROMs(roms, gen);
}
```

**Flow:**
```
BusReset IRQ → FSM → SelfIDCapture → TopologyManager::BuildTopology()
  → OnTopologyReady() → ROMScanner::Begin(gen, topology)
  → [ROM reads via AsyncSubsystem]
  → ROMScanner::CheckAndNotifyCompletion()
  → OnDiscoveryScanComplete(gen)
  → DeviceManager::ProcessROMs()
  → Enumerate devices to UserClient
```

## Error Handling

### UnrecoverableError Diagnostics
```cpp
void DiagnoseUnrecoverableError() {
    uint32_t hcControl = hw.ReadHCControl();
    uint32_t intEvent = hw.Read(kIntEvent);
    
    ASFW_LOG(Controller, "UnrecoverableError: HCControl=0x%08x IntEvent=0x%08x",
             hcControl, intEvent);
    
    // Check for paired errors (Linux pattern)
    if (intEvent & kPostedWriteErr) {
        ASFW_LOG(Controller, "Root cause: Posted write DMA failure");
        ASFW_LOG(Controller, "Common causes: Self-ID buffer, Config ROM IOMMU");
    }
    
    if (intEvent & kRegAccessFail) {
        ASFW_LOG(Controller, "Root cause: CSR register access failure");
    }
}
```

### Common Failure Modes

| Error | Symptoms | Cause | Fix |
|-------|----------|-------|-----|
| UnrecoverableError + postedWriteErr | Bus reset stalls, no Self-ID | Self-ID buffer DMA invalid | Arm buffer BEFORE linkEnable |
| UnrecoverableError alone | PHY communication failure | `programPhyEnable` not cleared | Clear gate after PHY config |
| selfIDComplete2 never fires | Topology stuck | Stale sticky bit | Clear in FSM (Linux pattern) |
| cycleTooLong | ISO timing violations | DMA overrun, system latency | Tune descriptor count |
| Bus reset storm | Repeated resets | ConfigROMheader=0 | Write real value to register |

## Thread Safety

| Component | Threading Model | Notes |
|-----------|----------------|-------|
| `ControllerCore` | Single-threaded | All calls via `scheduler->Queue()` |
| `HandleInterrupt()` | Dispatch queue | Serialized via `IOInterruptDispatchSource` |
| `ControllerStateMachine` | Atomic transitions | Lock-free reads, synchronized writes |
| `TopologySnapshot` | Immutable  | Created once per generation, read-only |
| `SharedStatusBlock` | Atomic updates | Sequence number for consistency check |

## Performance

- **Interrupt latency**: ~10-50μs (dispatch queue overhead)
- **Bus reset processing**: ~2-5ms (Self-ID → topology → discovery start)
- **Discovery scan**: ~200ms (bounded by ROM reads @ S100)
- **Config ROM staging**: ~1ms (DMA buffer setup + cache flush)

## Usage Example

```cpp
// 1. Construct with dependencies
ControllerCore::Dependencies deps{
    .hardware = hardware,
    .interrupts = interrupts,
    .scheduler = scheduler,
    .busReset = busReset,
    .topology = topology,
    .asyncSubsystem = async,
    .romScanner = romScanner,
    .deviceManager = deviceManager,
    // ... other dependencies
};

ControllerCore core(config, std::move(deps));

// 2. Start controller
kern_return_t kr = core.Start(provider);
if (kr != kIOReturnSuccess) {
    // Handle failure
}

// 3. Access stable interfaces
auto& bus = core.Bus();               // IFireWireBus facade
auto& dma = core.DMA();               // IDMAMemory facade

// 4. Query state
auto topology = core.LatestTopology();
if (topology) {
    uint32_t gen = topology->generation;
    uint8_t nodes = topology->nodeCount;
}

// 5. Stop on teardown
core.Stop();
```

## Design Patterns

### 1. **Dependency Injection**
All subsystems injected as shared_ptr, enabling:
- Unit testing with mocks
- Lazy initialization (nullptr check)
- Shared ownership across layers

### 2. **Interface Facades**
Stable API boundaries over internal engine:
```cpp
Bus() → IFireWireBus (hides AsyncSubsystem details)
DMA() → IDMAMemory (hides DMAMemoryManager details)
```

### 3. **Observer Pattern**
Topology callbacks decouple bus reset from discovery:
```cpp
busReset->BindCallbacks([](TopologySnapshot snap) {
    // Observer notified on topology ready
});
```

### 4. **FSM Delegation**
Bus reset complexity delegated to `BusResetCoordinator`:
- Single responsibility (ControllerCore = orchestrator, FSM = reset logic)
- Testable state machine in isolation
- Clear state transitions with logging

## Dependencies

- **Hardware**: `HardwareInterface` (OHCI register access)
- **Interrupts**: `InterruptManager` (IOInterruptDispatchSource wrapper)
- **Async**: `AsyncSubsystem` (AT/AR DMA engines)
- **Discovery**: `ROMScanner`, `DeviceManager` (device enumeration)
- **Bus**: `BusResetCoordinator`, `TopologyManager`, `SelfIDCapture`
- **Config ROM**: `ConfigROMBuilder`, `ConfigROMStager`
- **DriverKit**: `IOService`, `IODispatchQueue`, `IOBufferMemoryDescriptor`

## Design Goals

1. **Linux Compatibility**: Mirror `ohci_enable()` sequencing for hardware quirk resilience
2. **Apple Patterns**: FSM-driven, callback-based, immediate completion (vs polling)
3. **Modularity**: Clean dependency injection, testable subsystems
4. **Observability**: Rich logging, SharedStatusBlock for GUI, metrics tracking
5. **Correctness**: OHCI spec compliance (§5.7, §11, §13), error diagnostics
