# FireWire OHCI Isochronous Receive Architecture

This document describes how Apple's AppleFWOHCI driver implements isochronous receive (IR) functionality for FireWire OHCI controllers.

## Table of Contents
- [Architecture Overview](#architecture-overview)
- [Setup and Initialization](#setup-and-initialization)
- [DMA Context Management](#dma-context-management)
- [DCL Programs and Compilation](#dcl-programs-and-compilation)
- [Interrupt Handling](#interrupt-handling)
- [Hardware Interaction](#hardware-interaction)

---

## Architecture Overview

The isochronous receive subsystem is built on several key components:

### Core Classes

1. **AppleFWOHCI_DMAManager**
   - Central manager for all isochronous DMA contexts
   - Allocates and manages both transmit and receive contexts
   - Maintains context pools and resource allocation

2. **AppleFWOHCI_DCLProgram**
   - Represents a compiled DCL (Data Control Language) program
   - Manages descriptor lists for DMA operations
   - Handles interrupt processing and DCL execution

3. **AppleFWOHCI_ReceiveDCL / AppleFWOHCI_ReceiveDCL_U**
   - Individual receive DCL elements
   - Compile into OHCI INPUT_MORE/INPUT_LAST descriptors
   - Handle physical memory mapping and buffer management

4. **AppleFWOHCI_BufferFillIsochPort**
   - Simplified isochronous receive port for buffer-fill mode
   - Pre-allocates circular descriptor buffers
   - Lower overhead for streaming receive operations

5. **AppleFWOHCI_DMAManager::Context**
   - Represents a single hardware DMA context
   - Manages context state and OHCI register interactions
   - Provides start/stop/pause/resume functionality

---

## Setup and Initialization

### AppleFWOHCI::setupIsoch()

Located at address `0x29ce`, this function initializes the isochronous subsystem:

```c
int AppleFWOHCI::setupIsoch()
{
    // 1. Create dedicated workloop for isochronous operations
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop)
        return kIOReturnNoMemory;

    // 2. Configure real-time thread policy for timing-critical work
    //    - period: 625µs (5 FireWire cycles @ 125µs each)
    //    - computation: 60µs
    //    - constraint: 1.25ms
    mach_timespec_t policy[4];
    policy[0] = 625000ns;   // period
    policy[1] = 60000ns;    // computation
    policy[2] = 1250000ns;  // constraint
    policy[3] = 1;          // preemptible
    thread_policy_set(workloop_thread, THREAD_TIME_CONSTRAINT_POLICY, ...);

    // 3. Create DMA Manager
    fDMAManager = new AppleFWOHCI_DMAManager();
    if (!fDMAManager->init(this)) {
        fDMAManager->release();
        fDMAManager = NULL;
        return kIOReturnNoMemory;
    }

    // 4. Query hardware capabilities
    unsigned int rxContexts, txContexts;
    fDMAManager->getNumIsochContexts(&rxContexts, &txContexts);

    // 5. Publish context counts to registry
    setProperty("IsochReceiveContexts", rxContexts, 32);
    setProperty("IsochTransmitContexts", txContexts, 32);

    return kIOReturnSuccess;
}
```

**Key Points:**
- Real-time scheduling is critical for isochronous operations
- Period matches FireWire bus cycle time (125µs × 5 = 625µs)
- Context counts are hardware-dependent (typically 4 IR + 4 IT on OHCI 1.0)

---

## DMA Context Management

### Context Allocation

The `AppleFWOHCI_DMAManager::allocateReceiveContext()` function (at `0xa062`) manages IR context allocation:

```c
Context* allocateReceiveContext(unsigned int channel, IOFWIsochResourceFlags flags)
{
    // Special handling for buffer-fill contexts
    if (flags == kIOFWIsochResourceBufferFill ||
        flags == kIOFWIsochResourceBufferFillMultiChannel)
    {
        // Allocate from general pool
        for (int i = 0; i < numRxContexts; i++) {
            Context* ctx = &rxContexts[i];
            if (!ctx->inUse && ctx != dedicatedBufferFillContext)
                return ctx;
        }
    }

    // Check dedicated buffer-fill context
    Context* dedicatedCtx = dedicatedBufferFillContext;
    if (!dedicatedCtx->inUse ||
        (dedicatedCtx->type == kIOFWIsochResourceBufferFill &&
         flags <= kIOFWIsochResourceBufferFillMultiChannel))
    {
        return dedicatedCtx;
    }

    return NULL;  // No contexts available
}
```

**Context Types:**
- **Regular DCL contexts**: For program-driven receive
- **Buffer-fill contexts**: For simple streaming receive
- **Multi-channel contexts**: Can receive from multiple isochronous channels

### Context Start

The `Context::start()` function (at `0xa612`) activates a receive context:

```c
int Context::start()
{
    AppleFWOHCI* ohci = dmaManager->ohci;

    IOSimpleLockLock(lock);

    if (isReceive) {
        // Set IR context active bit
        ohci->writeRegister(kOHCIIRContextControlSet,
                           0,
                           1 << contextNumber);

        // Enable receive DMA
        ohci->writeRegister(kOHCIIsoRecvIntMaskSet,
                           0,
                           0x40);  // Enable IR DMA wake
    } else {
        // Transmit context setup
        ohci->writeRegister(kOHCIITContextControlSet,
                           0,
                           1 << contextNumber);

        ohci->writeRegister(kOHCIIsoXmitIntMaskSet,
                           0,
                           0x80);  // Enable IT DMA wake
    }

    // Write command pointer to start DMA
    *(uint32_t*)getHWContext() = 0x8000;  // Set ACTIVE flag

    IOSimpleLockUnlock(lock);

    return kIOReturnSuccess;
}
```

**OHCI Register Offsets:**
- `kOHCIIRContextControlSet` = base + 148 (0x94)
- `kOHCIIsoRecvIntMaskSet` = base + 136 (0x88)
- Context-specific control registers at offsets based on context number

---

## DCL Programs and Compilation

### DCL Architecture

DCL (Data Control Language) programs describe the data flow for isochronous operations. Each program consists of:

- **Send DCL**: Transmit descriptors
- **Receive DCL**: Receive descriptors
- **Skip Cycle DCL**: Skip one or more isochronous cycles
- **Branch**: Control flow between DCL elements

### Receive DCL Compilation

The `AppleFWOHCI_ReceiveDCL::compile()` function (at `0xc962`) converts high-level DCL into OHCI descriptors:

```c
int AppleFWOHCI_ReceiveDCL::compile(IODCLProgram* program, bool* needsUpdate)
{
    // 1. Determine descriptor type
    bool isHeader = (program->speed == kFWSpeed100MBit && headerBytes <= 7);
    int maxRanges = isHeader ? 6 : 7;
    int descriptorType = isHeader ? 1 : 0;

    if (numRanges > maxRanges)
        return kIOReturnNoMemory;

    // 2. Convert virtual addresses to physical
    if (!physicalRanges) {
        PhysicalSegment segments[8];
        unsigned int segmentCount = numRanges;

        program->virtualToPhysical(ranges, rangeCount,
                                  segments, &segmentCount, maxRanges);
        physicalRanges = segments;
    }

    // 3. Allocate OHCI descriptors
    allocDCLDescriptors(program, this, descriptorType,
                       1, physicalRanges, numRanges, needsUpdate);

    // 4. Build descriptor list
    uint32_t* desc = descriptorBlock->descriptors;

    if (isHeader) {
        // Header descriptor for small packets
        desc[0] = (8 - headerBytes) |           // Count
                 (waitForSync ? 0x30000 : 0) |  // Wait flags
                 (hasCallback ? 0x28000000 : 0x20000000);  // Status
        desc[1] = headerPhysAddr;
        desc[3] = 0;
        desc += 4;
    } else {
        // First packet data is in ranges[0]
        firstDescriptor = ranges[0][0];
    }

    // 5. Build intermediate INPUT_MORE descriptors
    for (int i = 0; i < numRanges - 1; i++) {
        desc[0] = physicalRanges[i].length |
                 (hasCallback ? 0x28000000 : 0x20000000);  // INPUT_MORE
        desc[1] = physicalRanges[i].address;
        desc[2] = 0;
        desc[3] = 0;
        desc += 4;
    }

    // 6. Build final INPUT_LAST descriptor
    PhysicalSegment* lastSeg = &physicalRanges[numRanges - 1];
    if (lastSeg->length == 0) {
        // Use dummy buffer for status
        lastSeg->length = 4;
        lastSeg->address = program->dummyPhysAddr;
    }

    desc[0] = lastSeg->length |
             (hasStatus ? 0x300000 : 0) |           // Store status
             (hasCallback ? 0x38000000 : 0x30000000);  // INPUT_LAST + interrupt
    desc[1] = lastSeg->address;
    desc[3] = 0;

    // 7. Update branching
    descriptorBlock->branchAddr = &desc[3];
    descriptorBlock->needsInterrupt = hasCallback || hasStatus;

    program->setDCLNeedsInterrupt(this, hasCallback || hasStatus);

    return kIOReturnSuccess;
}
```

**OHCI Descriptor Format:**
```
Word 0: reqCount (0-15) | flags (16-31)
Word 1: Physical address
Word 2: Branch address
Word 3: Status / timestamp (written by hardware)

Flags:
  0x20000000 = INPUT_MORE (continue)
  0x30000000 = INPUT_LAST (end of packet)
  0x28000000 = INPUT_MORE + interrupt
  0x38000000 = INPUT_LAST + interrupt
  0x00300000 = Store packet status
  0x00030000 = Wait for sync
```

### Buffer-Fill Mode

For simpler streaming scenarios, `AppleFWOHCI_BufferFillIsochPort` provides optimized descriptor generation:

```c
int AppleFWOHCI_BufferFillIsochPort::writeDescriptors()
{
    for (int bufIdx = 0; bufIdx < numBuffers; bufIdx++) {
        uint32_t* descBase = descriptorMemory->getVirtualAddress();
        uint32_t pageOffset = 16;
        uint32_t pageIndex = 0;

        uint32_t bufferSize = totalBufferSize / numBuffers;
        uint32_t descriptorCmd = bufferSize | 0x28000000;  // INPUT_MORE + IRQ

        for (int i = 0; i < numBuffers; i++) {
            pageOffset += 16;
            if (pageOffset >= page_size) {
                pageIndex++;
                pageOffset -= page_size;
            }

            uint32_t* desc = &descBase[i * 4];
            desc[0] = descriptorCmd;
            desc[1] = 0;  // Filled at runtime
            desc[2] = physicalPages[pageIndex] + pageOffset | 1;  // Branch + Z=1
            desc[3] = 0;
        }

        // Last descriptor branches to first
        descBase[numBuffers * 4 + 2] = 0;
    }

    return kIOReturnNotPermitted;  // Status code, not error
}
```

**Optimization:** Circular descriptor buffer allows continuous reception without software intervention between cycles.

---

## Interrupt Handling

### DCL Program Interrupt Handler

The `AppleFWOHCI_DCLProgram::handleInterrupt()` function (at `0x7be8`) processes completed receive operations:

```c
void AppleFWOHCI_DCLProgram::handleInterrupt()
{
    int totalInterrupts = 0;
    OSSet* dclSet = updateList;

    // 1. Count pending interrupts
    unsigned int numDCLs = dclSet->getCount();
    for (int i = 0; i < numDCLs; i++) {
        IOFWDCL* dcl = (IOFWDCL*)dclSet->getObject(i);
        if (dcl->descriptorBlock->needsInterrupt) {
            totalInterrupts += dcl->checkForInterrupt() ? 1 : 0;
        }
    }

    // 2. Walk DCL chain starting from last known position
    IOFWDCL* currentDCL = lastInterruptDCL ?: firstDCL;
    IOFWDCL* nextDCL = currentDCL;
    bool moreWork = false;
    unsigned int safetyCounter = maxDCLCount;

    while (safetyCounter-- > 0) {
        bool hasInterrupt = currentDCL->descriptorBlock->needsInterrupt;

        if (hasInterrupt) {
            // Process this DCL's interrupt
            bool consumed = false;
            currentDCL->interrupt(&consumed, &nextDCL);

            if (consumed) {
                moreWork = true;
                totalInterrupts--;
                lastInterruptDCL = nextDCL;
                break;  // Found the active interrupt
            }
        } else {
            // No interrupt, follow branch
            nextDCL = currentDCL->getBranch();
        }

        // Stop if we've walked entire program or found all interrupts
        if (!hasInterrupt && !consumed)
            break;
        if (!nextDCL)
            break;

        currentDCL = nextDCL;
    }

    // 3. Check for any remaining DCLs that need servicing
    if (totalInterrupts > 0) {
        for (int i = 0; i < numDCLs; i++) {
            IOFWDCL* dcl = (IOFWDCL*)dclSet->getObject(i);
            if (dcl->descriptorBlock->needsInterrupt) {
                bool consumed = false;
                dcl->interrupt(&consumed, &nextDCL);
                if (consumed)
                    lastInterruptDCL = nextDCL;
            }
        }
    }

    lastInterruptDCL = lastInterruptDCL;
}
```

**Interrupt Flow:**
1. Count how many DCLs have pending interrupts
2. Walk the DCL program chain from last known position
3. Process each DCL's completion callback
4. Update last interrupt position for next iteration
5. Handle any remaining unprocessed interrupts

### Buffer-Fill Interrupt

Simpler model for `AppleFWOHCI_BufferFillIsochPort`:

```c
int AppleFWOHCI_BufferFillIsochPort::handleInterrupt()
{
    // Delegate to DMA Manager's context interrupt handler
    return dmaManager->contextInterrupt(contextNumber);
}
```

**Callback Model:**
- User provides callback function during initialization
- Callback receives buffer ranges and packet count
- Simpler than full DCL program for streaming scenarios

---

## Hardware Interaction

### OHCI Register Map for IR Contexts

Each IR context has a dedicated register set (context N at offset base + N*16):

```
Offset  Register                    Description
------  --------                    -----------
0x400   IR0ContextControlSet        Context 0 control (set bits)
0x404   IR0ContextControlClear      Context 0 control (clear bits)
0x40C   IR0CommandPtr               Context 0 DMA command pointer
0x410   IR1ContextControlSet        Context 1 control (set bits)
...

0x088   IsoRecvIntMaskSet           IR interrupt mask (set bits)
0x08C   IsoRecvIntMaskClear         IR interrupt mask (clear bits)
0x090   IsoRecvIntEventSet          IR interrupt events (set bits)
0x094   IsoRecvIntEventClear        IR interrupt events (clear bits)
```

**Control Register Bits:**
- Bit 0-3: Context number
- Bit 10: Active
- Bit 11: Run
- Bit 15: Dead (error condition)
- Bit 16-31: Various control flags

### DMA Descriptor Chain

Hardware walks descriptor chain autonomously:

```
┌─────────────────┐
│ INPUT_MORE      │ ──┐
│ buf1: 1024 bytes│   │
└─────────────────┘   │
         │            │
         v            │
┌─────────────────┐   │
│ INPUT_MORE      │   │ Repeats for
│ buf2: 2048 bytes│   │ each buffer
└─────────────────┘   │ range
         │            │
         v            │
┌─────────────────┐   │
│ INPUT_LAST + IRQ│ ──┘
│ buf3: 512 bytes │
└─────────────────┘
         │
         v
    (interrupt fires, status written to word 3)
```

### Interrupt Policy

**When Interrupts Fire:**
1. **INPUT_LAST with interrupt bit**: End of packet reception
2. **Buffer-fill wrap**: Circular buffer completes one cycle
3. **Error conditions**: Dead context, descriptor read error
4. **Timestamp update**: Cycle time captured at reception

**Interrupt Coalescing:**
- Only INPUT_LAST descriptors typically generate interrupts
- INPUT_MORE used for multi-buffer packets without intermediate interrupts
- Reduces CPU overhead for large packets

---

## Performance Considerations

### Thread Policy

The real-time thread policy ensures:
- **Period**: 625µs matches 5 FireWire bus cycles
- **Computation budget**: 60µs for processing
- **Constraint**: 1.25ms maximum latency
- **Preemptible**: Can be interrupted for higher-priority work

### DMA Efficiency

- **Scatter-gather**: Multiple physical buffers per packet
- **Descriptor pre-allocation**: Compiled once, reused
- **Circular buffers**: Buffer-fill mode minimizes software overhead
- **Physical addressing**: No virtual→physical translation in hot path

### Robustness Features

The driver includes several robustness mechanisms:
- **Context death detection**: Hardware marks dead contexts
- **Descriptor validation**: Ensures valid physical addresses
- **Interrupt counting**: Detects missed or duplicate interrupts
- **Timeout handling**: Detects stuck DMA operations

---

## Summary

Apple's OHCI IR implementation uses a layered architecture:

1. **Hardware Layer**: OHCI DMA contexts and descriptors
2. **Management Layer**: DMAManager allocates and monitors contexts
3. **Program Layer**: DCL programs describe data flow
4. **API Layer**: Buffer-fill and DCL-based ports for different use cases

**Key Design Patterns:**
- Real-time scheduling for timing guarantees
- Descriptor pre-compilation for low latency
- Circular buffers for streaming efficiency
- Interrupt coalescing for reduced overhead
- Robust error detection and recovery

This architecture enabled reliable isochronous streaming for applications like audio/video capture, DV cameras, and high-speed disk interfaces over FireWire.
