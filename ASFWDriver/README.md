# ASFWDriver

## Overview

ASFWDriver is a comprehensive IEEE 1394 (FireWire) driver implementation for macOS using DriverKit. The driver provides full support for FireWire bus management, device discovery, asynchronous transactions, and protocol implementations following OHCI (Open Host Controller Interface) specifications.

## Architecture

The driver is organized into modular subsystems, each responsible for specific aspects of FireWire functionality:

### Core Subsystems

#### Async
The asynchronous transaction subsystem handles all non-isochronous FireWire communication:
- **Commands**: Various async command types (Read, Write, Lock, Phy)
- **Contexts**: Request/response context management (AR/AT)
- **Core**: Transaction management, DMA memory, completion strategies
- **Engine**: AT (Asynchronous Transmit) manager and context handling
- **Rx**: Receive path with packet parsing and routing
- **Tx**: Transmit path with descriptor and packet building
- **Track**: Transaction tracking, label allocation, and completion handling
- **Rings**: Buffer and descriptor ring management

#### Bus
Bus topology and management:
- Bus reset coordination and handling
- Self-ID capture and processing
- Topology management and tracking
- Generation tracking across bus resets
- Gap count optimization

#### Controller
Core controller functionality:
- Controller state machine
- Configuration management
- Hardware initialization and control

#### Discovery
Device discovery and management:
- Device registry and lifecycle management
- FireWire device and unit abstraction (`FWDevice`, `FWUnit`)
- Speed map and policy management

#### Hardware
Low-level hardware interface:
- OHCI register mapping and definitions
- Hardware interrupt management
- IEEE 1394 specifications and constants
- Descriptor formats and event codes

### Supporting Subsystems

#### ConfigROM
Configuration ROM handling:
- ROM building and staging
- ROM reading and scanning
- ROM storage and caching

#### IRM (Isochronous Resource Manager)
- IRM client implementation
- Resource allocation management
- Bus operations integration with async subsystem

#### Protocols
Protocol implementations built on top of the driver:
- **AVC**: Audio/Video Control protocol
  - Command handling
  - FCP (Function Control Protocol) transport
  - PCR (Plug Control Register) space management

#### Diagnostics
Comprehensive diagnostics and monitoring:
- Controller metrics collection
- Diagnostic logging
- Metrics sink for performance analysis

#### UserClient
User-space interface for applications:
- **Core**: Main user client implementation (`.iig` interface)
- **Handlers**: Specialized handlers for different operations:
  - Bus reset notifications
  - Config ROM access
  - Device discovery
  - Topology information
  - Transaction handling
  - Status queries
- **Storage**: Transaction state management
- **WireFormats**: Serialization formats for user-kernel communication

### Shared Components

#### Shared
Common components used across subsystems:
- Completion queue management
- DMA context management
- DMA memory allocation and policies
- Payload handling
- Ring buffer implementations (descriptor and buffer rings)

### Utilities

#### Common
Shared utilities and definitions (`FWCommon.hpp`, barrier utilities)

#### Logging
Centralized logging infrastructure

#### Scheduling
Task scheduling and coordination

#### Debug
Debugging tools (bus reset packet capture)

#### Testing
Test infrastructure and hooks for validation

#### Snapshot
System state snapshot utilities

## Key Design Patterns

### Asynchronous Transaction Flow
1. Commands are created with specific completion behaviors
2. Transactions are tracked via label allocation
3. DMA memory is managed through payload policies
4. Descriptors are built and submitted to hardware rings
5. Responses are parsed and routed to appropriate contexts
6. Completions are handled through registered callbacks

### Memory Management
- DMA memory pools with payload handles
- Ring buffers for descriptor and data management
- Efficient memory reuse through registries

### Context Management
- Separate AR (Asynchronous Receive) and AT (Asynchronous Transmit) contexts
- Request and response context isolation
- DMA context base classes for shared functionality

### Hardware Abstraction
- OHCI register abstraction layer
- Descriptor format encapsulation
- Event-driven interrupt handling

## Entry Points

- **ASFWDriver.cpp**: Main driver entry point
- **ASFWDriver.iig**: Driver interface definition
- **ASFWDriverUserClient.iig**: User client interface

## Dependencies

The driver is built using:
- DriverKit framework (macOS)
- OHCI 1.1 specification
- IEEE 1394-1995/IEEE 1394a-2000 standards

## Build Configuration

- **Info.plist**: Driver metadata and matching properties
- **ASFWDriver.entitlements**: Required driver entitlements

## Development

See [AGENTS.md](../../AGENTS.md) for development workflow, build instructions, and implementation guidelines.

For user client specific information, see [UserClient/README.md](UserClient/README.md).
