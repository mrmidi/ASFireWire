# ASOHCI Refactoring Plan & Checklist

This document tracks the progress of refactoring the ASOHCI DriverKit extension into a modular, maintainable, and spec-driven architecture. The goal is to establish a clean skeleton of classes with no-op methods, which can then be implemented systematically.

For specific code stubs and implementation notes, please see the corresponding section in [**REFACTOR_DETAILS.md**](REFACTOR_DETAILS.md).

**Note on Project Management:** For every new file created, the `ASFireWire.xcodeproj` must be updated to include the new `.hpp` and `.cpp` files in the `ASOHCI` target's "Sources" and "Headers" build phases. This is a critical step to ensure the project compiles.

---

## Phase 1: Skeleton File Creation

The first step is to create the empty files for our new class structure within the `ASFireWire/ASOHCI/` directory.

- [ ] **Core Logic**
    - [ ] `ASOHCIBusManager.hpp`
    - [ ] `ASOHCIBusManager.cpp`
    - [ ] `ASOHCIRegisterAccess.hpp`
    - [ ] `ASOHCIRegisterAccess.cpp`
    - [ ] `ASOHCICycleTimerService.hpp`
    - [ ] `ASOHCICycleTimerService.cpp`
- [ ] **Topology & Configuration**
    - [ ] `ASOHCITopologyDB.hpp`
    - [ ] `ASOHCITopologyDB.cpp`
    - [x] `ASOHCIConfigROM.hpp` (added)
    - [x] `ASOHCIConfigROM.cpp` (added)
    - [ ] `ASOHCICSRSpace.hpp`
    - [ ] `ASOHCICSRSpace.cpp`
- [ ] **DMA & Data Flow Management**
    - [ ] `ASOHCIAsyncManager.hpp`
    - [ ] `ASOHCIAsyncManager.cpp`
    - [x] AR design white paper: see `docs/AR_ASYNC_RECEIVE.md` for spec anchors and implementation notes.
    - [ ] `ASOHCIIsochManager.hpp`
    - [ ] `ASOHCIIsochManager.cpp`
    - [ ] `ASOHCIITContext.hpp`
    - [ ] `ASOHCIITContext.cpp`
    - [ ] `ASOHCIIRContext.hpp`
    - [ ] `ASOHCIIRContext.cpp`
    - [ ] `ASOHCIDMAProgramBuilder.hpp`
    - [ ] `ASOHCIDMAProgramBuilder.cpp`
- [ ] **Physical Requests & Utilities**
    - [ ] `ASOHCIAddressHandler.hpp`
    - [ ] `ASOHCIAddressHandler.cpp`
    - [ ] `ASOHCITrace.hpp`
    - [ ] `ASOHCITrace.cpp`

## Phase 2: Populate No-Op Stubs

Populate the newly created files with the C++ stub code. The code for each class can be found in [**REFACTOR_DETAILS.md**](REFACTOR_DETAILS.md).

- [ ] Paste stub code into all `.hpp` files.
- [ ] Paste stub code into all `.cpp` files.
- [x] Implemented `ASOHCIConfigROM` (BIB + minimal Root Dir + CRC-16).

## Phase 3: Update Xcode Project & Verify Build

Manually add all new files to the Xcode project file to ensure they are compiled as part of the `ASOHCI` dext. Then, verify the build from the command line.

**Warning:** Editing `project.pbxproj` is error-prone. Be careful with syntax, and ensure you have a backup.

- [ ] **Edit `ASFireWire/ASFireWire.xcodeproj/project.pbxproj`:**
    - [x] Added `ASOHCIConfigROM.hpp/.cpp` to `ASOHCI` target (Sources/Headers).
    - [ ] Locate the `PBXGroup` for the `ASOHCI` sources and add entries for remaining new files.
    - [ ] Locate the `PBXSourcesBuildPhase` for the `ASOHCI` target and add references to all remaining new `.cpp` files.
    - [ ] Locate the `PBXHeadersBuildPhase` for the `ASOHCI` target and add references to all remaining new `.hpp` files.
- [ ] **Verify Build:**
    - [ ] Run the command line build to confirm the project still compiles with the new files:
      ```sh
      xcodebuild -project ASFireWire.xcodeproj -target ASOHCI -configuration Debug build
      ```

## Phase 4: Integration into ASOHCI.cpp

Refactor the main `ASOHCI.cpp` file to use the new manager classes. The goal is to delegate responsibilities from `ASOHCI` to the new components.

- [ ] **Instance Variables:**
    - [ ] Add private member variables for all new manager classes in `ASOHCI.h`.
- [ ] **Initialization (`Start` method):**
    - [ ] Instantiate `ASOHCIBusManager` and `ASOHCICycleTimerService`.
    - [ ] Instantiate `ASOHCIConfigROM`, build the ROM, and attach it to `ASOHCICSRSpace`.
    - [ ] Instantiate `ASOHCIAddressHandler` and connect it to the `CSRSpace`.
    - [ ] Instantiate `ASOHCIAsyncManager` and bind it to the existing `AR`/`AT` contexts and the new `CSRSpace`.
    - [ ] Instantiate `ASOHCIIsochManager` and its `IT`/`IR` contexts (placeholders).
    - [x] Current integration: ROM buffer allocated/mapped; `ConfigROMmap` programmed; `BIBimageValid` set with LinkEnable.
    - [x] ROM image written big-endian; full hex dump added (trimmed), CRCs validate.
- [ ] **Teardown (`Stop` method):**
    - [ ] Add calls to the `onStop()` methods of the new manager classes.
    - [ ] Ensure all newly allocated objects are properly released.
- [ ] **Interrupt Handling (`InterruptOccurred_Impl`):**
    - [ ] **Bus Reset:** Delegate to `ASOHCIBusManager::handleBusResetIRQ()`.
    - [ ] **Self-ID Complete:** Delegate to `ASOHCIBusManager::handleSelfIDCompleteIRQ()`.
    - [ ] **DMA (AR/AT/IR/IT):** Delegate to the `handleInterrupt()` method of the corresponding context object (`ASOHCIARContext`, `ASOHCIATContext`, etc.).
    - [ ] **Posted Write Error:** Delegate to `ASOHCIAddressHandler::onPostedWriteError()`.
    - [x] Current: Commit `BusOptions` then `ConfigROMhdr` on BusReset (OHCI §5.5 parity).
    - [x] Current: Deferred cycle timer policy — enable after first stable Self‑ID; stop/restart AT on BusReset; explicit BusReset event clear.
    - [x] Current: Interrupt diagnostics added (bound type); synthetic IRQ self‑test used during bring‑up (to be removed once stable).

## Phase 5: Implementation Checklist

This section tracks the implementation of the `TODO` items within each new class.

### 1. Bus / Link State (`ASOHCIBusManager`) ([details](REFACTOR_DETAILS.md#1-bus--link-state-management))
- [ ] `onStart()`: Implement OHCI §5.7 link state sequencing and §11.x Self-ID flow.
- [ ] `onStop()`: Implement IRQ masking, `HCControl` clear per §6, §5.7.
- [ ] `handleBusResetIRQ()`: Implement §6.1 BusReset logic, §11 Self-ID timing.
- [ ] `handleSelfIDCompleteIRQ()`: Implement §11.5 logic, read Self-ID count, parse packets, and re-arm.
- [ ] `considerGapCountUpdate()`: Implement gap count update logic per OHCI §5.7 & 1394-2008 §8.6.2.
 - [ ] Initiate PHY bus reset (IBR) after bring‑up to kick Self‑ID on idle buses. [Deferred — not triggered in Start()]
 - [x] Self‑ID generation consistency check across parse (pre/post generation compare).
 - [ ] Assert `cycleMaster` alongside `cycleTimerEnable` post stable Self‑ID (policy gate: only when root?).

### 2. Topology Database (`ASOHCITopologyDB`) ([details](REFACTOR_DETAILS.md#4-self-id--topology-database))
- [ ] `ingestSelfIDs()`: Connect to `SelfIDParser` output and populate the node database.
- [ ] `probableRoot()`: Implement logic to derive the bus root node from port status.

### 3. Configuration ROM (`ASOHCIConfigROM`) ([details](REFACTOR_DETAILS.md#5-configuration-rom-builder))
- [x] `buildFromHardware()`: Build BIB (5 quadlets) + minimal Root Dir (Vendor_ID, Node_Capabilities).
- [x] `crc16()`: Implement ITU‑T CRC‑16 per IEEE 1212 §7.3 for BIB and directories.
- [x] Stage ROM header/BusOptions and commit on BusReset (write BusOptions, then Header).
- [ ] Atomic ROM updates: write `ConfigROMmap` (next) and trigger bus reset to swap images.

### 4. CSR Space (`ASOHCICSRSpace`) ([details](REFACTOR_DETAILS.md#6-csr-space-façade))
- [ ] `read()`: Implement CSR read handling for physical requests per 1212 §5.1 + §6.
- [ ] `write()`: Implement CSR write handling with STATE_CLEAR/SET semantics per 1212 §6.1–§6.5.
- [ ] `lock()`: Implement lock transaction handling per 1212 §5.2.

### 5. Async DMA (`ASOHCIAsyncManager`) ([details](REFACTOR_DETAILS.md#7-asynchronous-requestresponse-orchestration))
- [ ] `onARPacketAvailable()`: Parse incoming packet headers (§8.7) and route to CSR or user client.
- [ ] `onATComplete()`: Handle transmit complete interrupts (§7.6), including error checking and retries (§7.4).
- [ ] `queueAsyncResponse()`: Build response packets using `ASOHCIDMAProgramBuilder` (§7.1, §7.8).
 - [ ] AR ring recycle: retire/requeue buffers; header‑peek logging; stop/restart on BusReset.
 - [ ] AT minimal OUTPUT_LAST Immediate (header‑only) enablement for future CSR replies.

### 6. Isochronous DMA (`ASOHCIIsochManager`, `IT`/`IR` Contexts) ([details](REFACTOR_DETAILS.md#8-isochronous-orchestration))
- [ ] `openIT()`: Implement IT context initialization (§9.2).
- [ ] `queueITPacket()`: Build IT descriptor chains (§9.1).
- [ ] `closeIT()`: Stop and drain the IT context (§3.1).
- [ ] `openIR()`: Implement IR context initialization (§10.3).
- [ ] `recycleIRBuffers()`: Manage buffer replenishment using program appending (§10.1).
- [ ] `closeIR()`: Stop and drain the IR context.
- [ ] `ASOHCIITContext::handleInterrupt()`: Implement IT interrupt handling (§9.5).
- [ ] `ASOHCIIRContext::handleInterrupt()`: Implement IR interrupt handling (§10.5).

### 7. DMA Program Builder (`ASOHCIDMAProgramBuilder`) ([details](REFACTOR_DETAILS.md#10-dma-descriptor-micro-builder))
- [ ] `initOutputMore()`: Implement descriptor encoding.
- [ ] `initOutputLast()`: Implement descriptor encoding.
- [ ] `initInputMore()`: Implement descriptor encoding.
- [ ] `initInputLast()`: Implement descriptor encoding.

### 8. Physical Address Handling (`ASOHCIAddressHandler`) ([details](REFACTOR_DETAILS.md#11-physical-request-filtering))
- [ ] `onPostedWriteError()`: Implement host bus error diagnostics per §13.
