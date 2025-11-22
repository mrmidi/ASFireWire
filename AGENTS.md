# AGENTS.md - AI Assistant Guide for ASFW Project

This document provides guidance for AI assistants (like Claude) when working with the ASFW codebase. It covers architecture patterns, coding conventions, and important context about the project structure.

## Project Overview

ASFW is a modern macOS FireWire (IEEE 1394) driver implementation using DriverKit. Since macOS Tahoe (26), Apple completely removed the FireWire stack. This project aims to restore FireWire functionality using modern user-space DriverKit APIs instead of traditional kernel extensions.

**Key Technologies:**

- DriverKit and PCIDriverKit frameworks (user-space driver architecture)
- Modern C++23 with strong type safety
- Swift 6 with strict concurrency for the control app
- OHCI (Open Host Controller Interface) specification for FireWire hardware

## Code Architecture & Patterns

### 1. Strong Type Safety

The codebase emphasizes compile-time type safety to prevent subtle bugs in low-level hardware programming. Examples:

**ASFWDriver/Core/PhyPackets.hpp** - PHY packet types:

```cpp
struct AlphaPhyConfig {
    std::uint8_t rootId{0};
    bool forceRoot{false};
    bool gapCountOptimization{false};
    std::uint8_t gapCount{0x3F};

    [[nodiscard]] constexpr Quadlet EncodeHostOrder() const noexcept;
    [[nodiscard]] constexpr std::array<Quadlet, 2> EncodeBusOrder() const noexcept;
};
```

Benefits:

- Type-safe PHY packet construction prevents bit manipulation errors
- constexpr methods enable compile-time validation
- Explicit endianness conversion (ToBusOrder/FromBusOrder) prevents byte order bugs
- [[nodiscard]] prevents accidentally ignoring return values

**Context Management Patterns:**

- CRTP (Curiously Recurring Template Pattern) for zero-overhead polymorphism in hot paths
- Template parameters enforce compile-time type differentiation
- std::expected for error handling (C++23) - no exceptions in driver code
- RAII patterns for resource management (locks, DMA buffers, etc.)

### 2. Modern C++23 Features

The codebase leverages cutting-edge C++ features:

- **std::expected**: Error handling without exceptions (critical for drivers)
- **std::span**: Safe array views without ownership
- **std::byteswap**: Explicit endianness conversion (PhyPackets.hpp:20)
- **constexpr**: Compile-time computation and validation
- **Concepts**: Template constraints for type safety
- **std::unique_ptr/shared_ptr**: Automatic resource management
- **[[nodiscard]]**: Prevent ignoring critical return values

### 3. CRTP for Static Polymorphism (Optional Pattern)

CRTP is used in some contexts for compile-time type safety, but **it is not strictly required**. The primary benefit is catching bugs at compile time instead of silent runtime failures on the bus.

```cpp
template<typename Derived, ContextRole Tag>
class ContextBase {
    // Zero-overhead polymorphism for context-specific behavior
    // Used in asynchronous/isochronous transaction processing
};
```

**Primary benefit of CRTP:**

- **Compile-time bug detection** - Type mismatches caught by compiler instead of silent bus errors
- Enforces correct context role usage (AT Request vs AT Response, etc.)
- Prevents mixing incompatible context types at compile time

**Use virtual methods/interfaces when:**

- Runtime polymorphism is needed
- Interface flexibility is more important than compile-time guarantees

### 4. Memory Management & RAII

All resources use RAII patterns:

```cpp
~ATContextBase() {
    if (submitLock_) {
        IOLockFree(submitLock_);
        submitLock_ = nullptr;
    }
}
```

- IOLock wrappers for DriverKit synchronization primitives
- DMA buffer allocation/deallocation in constructors/destructors
- No manual resource tracking

### 5. Endianness Handling

FireWire uses big-endian on the wire, but OHCI expects little-endian descriptor headers with big-endian payloads. All endianness conversions may be explicit:

```cpp
constexpr Quadlet ToBusOrder(Quadlet value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(value);
    }
    return value;
}
```

**Critical Rule:** Always verify endianness. Use packet analyzer for validation.

## Project Structure

### Repository Layout

- **ASFW/** - Control app and installer (Swift/SwiftUI)
  - Modern Swift 6 with strict concurrency enabled
  - User-facing installation and debugging utilities
  - Required for installing DriverKit-based drivers on macOS

- **ASFWDriver/** - Main DriverKit-based FireWire driver (detailed below)

- **tests/** - Unit and integration tests
  - Tests driver logic WITHOUT requiring DriverKit dependencies
  - Isolates testable code from hardware/DriverKit APIs
  - **Note:** Does NOT test DriverKit interactions themselves (requires hardware)

- **tools/** - Build and development utilities

### ASFWDriver Structure

The driver is organized into functional subsystems:

**Core Components:**

- **Core/** - Hardware interface, controller state machine, topology management
  - `OHCIConstants.hpp` - Centralized hardware register definitions (single source of truth)
  - `PhyPackets.hpp` - Type-safe PHY packet construction
  - `HardwareInterface` - MMIO register access abstraction
  - `ControllerCore` - Main controller initialization and lifecycle
  - `TopologyManager` - Bus topology tracking from Self-ID packets
  - `BusManager` - Bus management and IRM functionality
  - `ConfigROMBuilder/Stager` - Configuration ROM generation and activation
  - `InterruptManager` - Hardware interrupt handling and dispatch

**Async Subsystem:**

- **Async/** - Asynchronous packet transmission/reception
  - **Commands/** - High-level async operations (Read, Write, Lock, Phy)
  - **Contexts/** - OHCI DMA context wrappers (AT/AR Request/Response)
  - **Core/** - Transaction management, DMA memory, payload handling
  - **Engine/** - Context managers and DMA engine coordination
  - **Rings/** - Descriptor and buffer ring implementations
  - **Tx/** - Descriptor/packet builders, submission logic
  - **Rx/** - Packet parsing, routing, receive path handling
  - **Track/** - Transaction tracking, label allocation, completion queues
  - **Interfaces/** - Abstract interfaces for testability (IDMAMemory, IFireWireBus)

**Device Discovery:**

- **Discovery/** - Device enumeration and Config ROM reading
  - `DeviceManager` - Device lifecycle and registry management
  - `ROMReader/Scanner` - Config ROM reading and parsing
  - `FWDevice/FWUnit` - Device and unit directory representations
  - `ConfigROMStore` - Cached ROM data storage

**IRM (Isochronous Resource Manager):**

- **IRM/** - Bandwidth and channel allocation
  - `IRMClient` - IRM election and resource management
  - `IRMAllocationManager` - Channel/bandwidth allocation tracking

**Protocol Support:**

- **Protocols/AVC/** - Audio/Video Control protocol
  - `FCPTransport` - FCP (Function Control Protocol) implementation
  - `AVCDiscovery` - AV/C device discovery
  - `AVCUnit` - AV/C unit and subunit management
  - `PCRSpace` - Plug Control Register handling

**User Client:**

- **UserClient/** - DriverKit user-space client interface
  - **Core/** - User client implementation (ASFWDriverUserClient.iig)
  - **Handlers/** - Request handlers for transactions, topology, device discovery
  - **WireFormats/** - Serialization formats for user-kernel communication

**Utilities:**

- **Base/** - Common utilities (StatusOr for error handling)
- **Logging/** - Structured logging system
- **Debug/** - Diagnostic tools (packet capture, tracing)
- **Snapshot/** - System state snapshots for debugging

### Documentation Structure

**Two documentation folders with distinct purposes:**

1. **docs/** - Internal reference documentation (contains copyrighted specs)
   - `docs/linux/` - Linux FireWire driver implementation (reference source)
   - `docs/IOFireWireFamily/` - Apple's open-source FireWire kext (reference source)
   - `docs/IOFireWireAVC/` - Apple's AV/C protocol implementation (reference source)
   - `docs/ohci/` - OHCI specification materials
   - `docs/DriverKit/` - DriverKit SDK reference materials
   - These are excellent reference sources when implementing features

2. **documentation/** - Public documentation (project-generated)
   - API documentation, implementation guides, design decisions
   - Safe to share publicly

**When implementing features:**

- Consult Linux drivers (ASFW/docs/linux/) for proven OHCI patterns
- Check Apple's IOFireWireFamily (ASFW/docs/IOFireWireFamily/) for historical approaches
- Reference IOFireWireAVC (ASFW/docs/IOFireWireAVC/) for AV/C protocol details

## Coding Guidelines

### General Principles

1. **Type Safety First**
   - Use strong types instead of primitive types
   - constexpr for compile-time validation
   - static_assert for invariant checking

2. **Error Handling**
   - std::expected for recoverable errors
   - [[nodiscard]] on all error-returning functions
   - Document failure modes in comments

3. **Memory Safety**
   - RAII for all resources
   - std::span for array views
   - No raw pointer arithmetic unless interfacing with C APIs

4. **Modularity**
   - Single Responsibility Principle
   - Isolate DriverKit dependencies for testability
   - Avoid mega-classes

5. **Documentation**
   - OHCI spec section references in comments (e.g., "§7.2.3")
   - Apple/Linux pattern comparisons for reviewers
   - Rationale for non-obvious decisions

### Performance Considerations

1. **Centralize constants** - Single source of truth prevents subtle bugs (see OHCIConstants.hpp)
2. **Zero-copy where possible** - Use std::span and views instead of copying data
3. **DMA coherency** - Understand memory ordering requirements for hardware access

### Testing Strategy

- **Isolate logic from DriverKit** - Make code testable without hardware
- **Unit tests** - Test packet encoding/decoding, state machines, algorithms
- **Cannot test** - DriverKit interactions require actual hardware/OS integration
- **Use packet analyzer** - Hardware-level validation (OHCI errors are cryptic)

## Common Pitfalls & Gotchas

1. **Endianness is Critical**
   - OHCI descriptors: little-endian headers, big-endian payloads
   - Wire format: big-endian IEEE 1394 packets
   - Always use explicit conversion functions

2. **DMA Coherency**
   - Use proper memory barriers (OSSynchronizeIO, IoBarrier)
   - Flush descriptor changes before waking hardware
   - Fetch descriptor status before reading completion fields

3. **Bit Field Precision**
   - One missed bit shift can silently break packets
   - Use centralized constants (ASFWDriver/Core/OHCIConstants.hpp)
   - static_assert for invariants

4. **OHCI Timing**
   - Context stop/quiesce requires polling with timeout
   - Apple uses escalating delays (5µs → 255µs) for efficiency
   - Don't assume immediate hardware response

5. **Packet Analyzer is Essential**
   - OHCI event codes are often unhelpful (evt_unknown, etc.)
   - Packet analyzer shows actual wire-level errors
   - Example: PowerMac G3 with FireWire 400 as analyzer

## Swift Component (ASFW App)

The control application uses Swift 6 with strict concurrency:

- **Actor-based concurrency model** for thread safety
- **Sendable protocols** for cross-actor communication
- **SwiftUI** for modern declarative UI
- **System Extension** installation and management
- **Debugging utilities** for driver diagnostics

When working on the Swift app:

- Respect strict concurrency requirements
- Use proper actor isolation
- Handle async/await patterns correctly

## Building & Development

**Primary Build Method:** Xcode (required for proper signing)

**Build scripts (build.sh, CMakeLists.txt):**

- Generate compile_commands.json for static analysis
- Quick testing/iteration
- **Not for production builds**

## Current Status & Roadmap

**Working:**

- OHCI controller initialization
- Bus reset and Self-ID processing
- Basic asynchronous transfers (quadlet reads)
- DMA buffer management
- Interrupt handling
- Config ROM reading from attached devices

**In Progress / Planned:**

1. Additional async commands (block read/write, lock, PHY packets) (partially done)
2. AV/C protocol support (for audio interfaces like Apogee Duet) (in progress )
3. Isochronous transfers (audio/video streams) (planned)
4. IRM (Isochronous Resource Manager) support (partially done)

## Reference Materials

- IEEE 1394-2008 Standard - Complete FireWire specification
- OHCI 1.1 Specification - Hardware programming interface
- Linux firewire drivers (ASFW/docs/linux/) - Proven implementation patterns
- Apple IOFireWireFamily (ASFW/docs/IOFireWireFamily/) - Historical reference
- DriverKit headers - More accurate than official documentation

## Key Files to Understand

Start with these files to understand the codebase:

1. **README.md** - Project overview and context
2. **ASFWDriver/Core/PhyPackets.hpp** - Example of strong typing and compile-time validation
3. **ASFWDriver/Core/OHCIConstants.hpp** - Centralized hardware constants (single source of truth)
4. **ASFWDriver/Async/Contexts/** - Context management patterns (CRTP, RAII)
5. **ASFWDriver/Async/Core/DMAMemoryManager.hpp** - Memory management patterns

## Final Notes

This is a complex, low-level systems project involving:

- Hardware specification compliance (OHCI, IEEE 1394)
- Modern C++ systems programming
- macOS DriverKit framework
- DMA and memory coherency
- Real-time interrupt handling
- Endianness and bit-level manipulation

When in doubt:

1. Check the OHCI specification (ASFW/docs/ohci/)
2. Reference Linux/Apple implementations (ASFW/docs/linux/, ASFW/docs/IOFireWireFamily/)
3. Use a packet analyzer for validation
4. Consult the README.md for project context and design decisions
