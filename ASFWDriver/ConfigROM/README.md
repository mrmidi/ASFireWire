# Config ROM Subsystem

## Overview

The **ConfigROM** subsystem provides a comprehensive implementation of the IEEE 1394 Configuration ROM architecture. It handles both the **local presentation** (exposing the driver's capabilities to the bus) and **remote discovery** (scanning and parsing remote device capabilities).

The implementation strictly adheres to:
- **IEEE 1212-2001**: Standard for a Control and Status Registers (CSR) Architecture.
- **IEEE 1394-1995**: High Performance Serial Bus specification.
- **1394 OHCI 1.1**: Open Host Controller Interface specification (§5.5.6 Shadow ROM).

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
│  │  ROMScanner  │  │  ROMReader   │  │  ConfigROM   │  │
│  │ (Orchestrator)  │ (Transactions) │  │    Store     │  │
│  └──────┬───────┘  └──────────────┘  └──────────────┘  │
│         │          ┌──────────────┐  ┌──────────────┐  │
│  ┌──────▼───────┐  │  ConfigROM   │  │  ConfigROM   │  │
│  │ ROMScanSession  │  │   Builder    │  │    Stager    │  │
│  └──────────────┘  │   (Local)    │  │  (Hardware)  │  │
│                    └──────────────┘  └──────────────┘  │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│         AsyncSubsystem / HardwareInterface              │
│          (Async transactions, DMA buffers)              │
└─────────────────────────────────────────────────────────┘
```

## 1. Remote ROM Discovery (Read Path)

### ROMScanner & ROMScanSession
The `ROMScanner` is the top-level orchestrator for discovering remote nodes. It uses `ROMScanSession` to manage the lifecycle of a scan for a specific bus generation.

**Key Features:**
- **Bounded Concurrency**: Limits the number of in-flight ROM reads to prevent bus saturation (default: 4).
- **Session-Driven**: Each bus reset triggers a new session, ensuring late callbacks from previous generations are safely ignored.
- **FSM-Driven Discovery**: Each node progresses through a state machine (`ROMScanNodeStateMachine`):
  `Idle` → `ReadingBIB` → `VerifyingIRM` → `ReadingRootDir` → `ReadingDetails` → `Complete`.
- **IRM Verification**: Optionally verifies Isochronous Resource Manager capability by performing a `CompareSwap` test on the `CHANNELS_AVAILABLE_63_32` register.

### ROMReader
A specialized wrapper around `IFireWireBus` optimized for Config ROM access.
- **Quadlet-Only Reads**: Avoids block reads which are inconsistently implemented across FireWire hardware.
- **Big-Endian Handling**: All wire-format data is treated as Big-Endian and decoded using `ConfigROMParser`.
- **Address Mapping**: Targets the standard ROM base at `0xFFFFF0000400` (IEEE 1394-1995 §8.3.2).

### ConfigROMParser
A stateless utility class that decodes wire-format quadlets into C++ structures.
- **BIB Parsing**: Extracts GUID, `max_rec`, and link speed from the Bus Information Block.
- **Directory Decoding**: Recursively resolves IEEE 1212 Directory entries (Immediate, Leaf, and Directory types).
- **CRC-16**: Implements the IEEE 1212 CRC-16 polynomial (`0x1021`) (IEEE 1212 §7.3).

---

## 2. Local ROM Presentation (Write Path)

### ConfigROMBuilder
Constructs the 1 KB Configuration ROM image presented by the host controller.
- **Staged API**: Allows incremental building (`Begin` → `AddImmediateEntry` → `AddTextLeaf` → `Finalize`).
- **IEEE 1212 Structures**: Automatically manages the generation of the Bus Info Block and Root Directory.
- **Textual Descriptors**: Handles the encoding of "Minimal ASCII" leaves for vendor and model strings (IEEE 1212 §7.5.4.1).

### ConfigROMStager
Bridges the logical ROM image to the physical OHCI hardware registers.
- **DMA Management**: Allocates and maps a 1 KB buffer visible to the OHCI controller.
- **Shadow Loading (OHCI §5.5.6)**: 
  1. Writes native-endian ROM image to DMA buffer.
  2. Programs `BusOptions`, `GUIDHi`, `GUIDLo`, and `ConfigROMMap`.
  3. Writes to `ConfigROMHeader` to activate the shadow registers on the next bus reset.
- **Header Restoration**: Automatically restores the first quadlet of the ROM buffer after a bus reset, as hardware may zero it during the load process.

---

## 3. Storage & State Management

### ConfigROMStore
A persistent, thread-safe registry for all discovered ROMs.
- **Generation-Aware**: Tracks which ROMs are valid for the current topology.
- **GUID Deduplication**: Ensures only one entry exists per unique EUI-64.
- **Lifecycle Management**: 
  - **Suspended**: ROMs from a previous generation waiting for validation.
  - **Validated**: ROMs confirmed to still exist after a bus reset.
  - **Invalid**: Devices that have disappeared from the bus.

---

## Specification Tracing

| Feature | Specification | Code Component |
| :--- | :--- | :--- |
| Bus Information Block | IEEE 1394-1995 §8.3.2.5 | `ConfigROMParser::ParseBIB` |
| Directory Entries | IEEE 1212 §7.5.1 | `ConfigROMParser::ParseDirectory` |
| Textual Leaves | IEEE 1212 §7.5.4.1 | `ConfigROMBuilder::AddTextLeaf` |
| CRC-16 Algorithm | IEEE 1212 §7.3 | `ConfigROMParser::ComputeCRC16_1212` |
| Shadow Registers | OHCI 1.1 §5.5.6 | `ConfigROMStager::StageImage` |
| IRM Capability | IEEE 1394a-2000 §8.3.2.3.10 | `ROMScanSession::StartIRMRead` |

## Design Patterns

### FSM-Driven Async Flow
Because ROM discovery involves multiple high-latency bus transactions, the subsystem uses a non-blocking asynchronous approach. `ROMScanSession` pumps the state machines of all nodes in parallel, using completion callbacks to advance states.

### Quadlet-Only Transaction Policy
To maximize compatibility with legacy or "quirky" hardware, the `ROMReader` never uses block read transactions for Configuration ROM. Instead, it reads individual 32-bit quadlets, as some devices have buggy or inconsistent block-read implementations.

### Ownership & Thread Safety
- `ROMScanner` and `ROMScanSession` are strictly single-threaded, typically running on the driver's primary `IODispatchQueue`.
- `ConfigROMStore` is protected by an `IOLock` to allow concurrent lookups from different contexts (e.g., Discovery and UserClient).
