# ASFireWire Development Plan: Complete OHCI Implementation

## Current Status ✅

### **Major Breakthrough Achieved**
- [x] **PHY register access fully working** - OHCI-compliant implementation complete
- [x] **Device detection successful** - FireWire devices connecting properly  
- [x] **OHCI-compliant initialization** - 10-phase sequence implemented
- [x] **Comprehensive interrupt logging** - All OHCI interrupt bits decoded with spec references

### **Working Features**
- [x] Software reset sequence (OHCI §5.7)
- [x] Link Power Status (LPS) initialization 
- [x] PHY register read/write operations (OHCI §5.12)
- [x] Self-ID packet parsing (IEEE 1394-2008)
- [x] Bus reset detection and handling
- [x] PHY scan with port status enumeration
- [x] MSI interrupt configuration
- [x] Detailed interrupt bit analysis with OHCI spec references
- [x] Basic AR/AT DMA contexts initialized and started (foundation; handlers minimal)

## Issues Identified from Current Logs

### **1. Repeated Bus Resets & Cycle Inconsistencies**
- **Pattern**: Continuous bus resets every ~170ms with "Cycle Inconsistent" interrupts  
- **Log Evidence**: `IntEvent=0x00800000` (Cycle Inconsistent bit 23)
- **Root Cause**: Missing DMA context initialization and AR/AT context setup
- **Linux Reference**: Lines 2583-2584 in ohci.c - `ar_context_run()` calls missing

### **2. DMA Contexts: Foundation Implemented**
- **Current State**: AR (Receive) and AT (Transmit) context classes implemented and started
- **Remaining**: AT descriptor management and packet queueing; AR/AT interrupt processing logic
- **References**: OHCI §7/§8 for descriptor programs; Linux `ar_context_run()` / `at_context_run()` for patterns (rewrite only)

### **3. Missing Configuration ROM Setup**
- **Current Gap**: No Config ROM implementation  
- **Linux Reference**: Lines 2537-2558 in ohci_enable() - Config ROM allocation and setup
- **OHCI Spec**: §5.5 Autonomous CSR Resources

### **4. Incomplete IEEE 1394a Enhancement Configuration**
- **Current Status**: Basic aPhyEnhanceEnable set, but missing PHY register programming
- **Linux Reference**: configure_1394a_enhancements() function (lines 2346-2395)
- **Missing**: PHY register 5 programming for acceleration and multi-speed

## Implementation Roadmap

### **Phase A: DMA Context Foundation**

#### **A1. Study Linux AR/AT Context Architecture** 
- [ ] Analyze `struct ar_context` in ohci.c (line 95+)
- [ ] Analyze `struct at_context` in ohci.c  
- [ ] Understand DMA descriptor chain management
- [ ] Review buffer allocation and management strategies
- [ ] Study `ar_context_run()` function implementation

#### **A2. Read OHCI DMA Specifications**
- [ ] Study [§7 Asynchronous Transmit DMA](../docs/ohci/69-7-asynchronous-transmit-dma.pdf) 
  - [ ] [§7.1 AT DMA Context Programs](../docs/ohci/71-at-dma-context-programs.pdf)
  - [ ] [§7.6 AT Interrupts](../docs/ohci/76-at-interrupts.pdf)
- [ ] Study [§8 Asynchronous Receive DMA](../docs/ohci/95-8-asynchronous-receive-dma.pdf)
  - [ ] [§8.1 AR DMA Context Programs](../docs/ohci/81-ar-dma-context-programs.pdf)
  - [ ] [§8.2 bufferFill mode](../docs/ohci/82-bufferfill-mode.pdf)
- [ ] Study [§3 Common DMA Controller Features](../docs/ohci/17-3-common-dma-controller-features.pdf)
  - [ ] [§3.1 Context Registers](../docs/ohci/31-context-registers.pdf)
  - [ ] [§3.2 List Management](../docs/ohci/32-list-management.pdf)

#### **A3. Implement Basic AR Context**
- [x] Create ASOHCIARContext class for Asynchronous Receive
- [x] Implement buffer pool management with IOBufferMemoryDescriptor
- [x] Add DMA descriptor chain handling
- [x] Implement `ar_context_run()` equivalent
- [x] Add AR interrupt handlers (basic logging; full processing TBD)

#### **A4. Implement Basic AT Context**  
- [x] Create ASOHCIATContext class for Asynchronous Transmit
- [ ] Implement AT DMA descriptor management
- [x] Add AT interrupt handlers (basic logging; full processing TBD)
- [x] Implement `at_context_run()` equivalent (context runs with empty program)

### **Phase B: Configuration ROM Implementation**

#### **B1. Study OHCI Config ROM Requirements**
- [ ] Read [§5.5 Autonomous CSR Resources](../docs/ohci/55-autonomous-csr-resources.pdf)
- [ ] Analyze Linux config_rom handling in ohci_enable() (lines 2537-2558)
- [ ] Study ConfigROMmap, ConfigROMhdr, BusOptions register usage

#### **B2. Implement Config ROM Support**
- [ ] Add Config ROM buffer allocation using IOBufferMemoryDescriptor
- [ ] Implement ConfigROMmap register programming
- [ ] Implement ConfigROMhdr and BusOptions register setup
- [ ] Add atomic config ROM update mechanism
- [ ] Add BIBimageValid bit handling

### **Phase C: Complete IEEE 1394a Enhancement**

#### **C1. Implement Missing PHY Programming**
- [ ] Add IEEE 1394a capability detection (PHY register 2)
- [ ] Implement PHY register 5 acceleration/multi-speed configuration
- [ ] Add paged PHY register access support (Linux read_paged_phy_reg)
- [ ] Study Linux configure_1394a_enhancements() implementation

#### **C2. Complete Link Enhancement Configuration**  
- [ ] Implement proper programPhyEnable/aPhyEnhanceEnable sequencing
- [ ] Add PHY and Link consistency checks
- [ ] Clear programPhyEnable after configuration (Linux line 2391-2392)

### **Phase D: Interrupt and Error Handling**

#### **D1. Implement Missing Interrupt Handlers**
- [ ] AT/AR DMA completion interrupts
- [ ] Physical request/response handling (OHCI §12)
- [ ] Isochronous context interrupts (§9, §10)
- [ ] regAccessFail interrupt handling
- [ ] unrecoverableError interrupt handling

#### **D2. Add Comprehensive Error Recovery**
- [ ] Bus reset recovery procedures
- [ ] DMA error handling and context restart
- [ ] PHY error recovery
- [ ] Cycle timer error handling

### **Phase E: Advanced Features**

#### **E1. Isochronous Support**
- [ ] Study [§9 Isochronous Transmit DMA](../docs/ohci/111-9-isochronous-transmit-dma.pdf)
- [ ] Study [§10 Isochronous Receive DMA](../docs/ohci/129-10-isochronous-receive-dma.pdf)
- [ ] Implement IT/IR context management

#### **E2. Physical Request Support**
- [ ] Study [§12 Physical Requests](../docs/ohci/151-12-physical-requests.pdf)
- [ ] Implement physical request filtering
- [ ] Add posted write support

## Technical References

### **Linux ohci.c Key Functions**
- `ohci_enable()` (lines 2419-2592) - Main initialization sequence  
- `configure_1394a_enhancements()` (lines 2346-2395) - IEEE 1394a setup
- `software_reset()` (lines 2317-2335) - Reset sequence reference
- `ar_context_run()` & `at_context_run()` - DMA context activation
- PHY register access functions (lines 630-750)

### **OHCI Specifications** 
See [docs/ohci/README.md](../docs/ohci/README.md) for complete specification index.

**Critical Sections:**
- §3: Common DMA Controller Features
- §5.7: HCControl registers  
- §5.12: PHY control register
- §7: Asynchronous Transmit DMA
- §8: Asynchronous Receive DMA
- §11: Self ID Receive

### **Current ASFireWire Implementation**
- **ASOHCI/ASOHCI.cpp**: Main controller implementation
- **ASOHCI/PhyAccess.cpp**: PHY register access (✅ complete)
- **ASOHCI/OHCIConstants.hpp**: OHCI register definitions
- **ASOHCI/SelfIDParser.cpp**: Self-ID packet parsing
- **ASOHCI/ASOHCIARContext.*:** Asynchronous Receive context (buffers + descriptors)
- **ASOHCI/ASOHCIATContext.*:** Asynchronous Transmit context (foundation)

## Success Criteria

### **Immediate Goals**
- [ ] **Eliminate repeated bus resets** - Stable bus topology maintained
- [ ] **Stop "Cycle Inconsistent" interrupts** - Proper cycle timer management
- [x] **Enable AR/AT DMA contexts** - Foundation for data transfer

### **Intermediate Goals**
- [ ] **Proper device enumeration** - FireWire devices fully recognized and stable
- [ ] **Configuration ROM functional** - Device configuration data available
- [ ] **IEEE 1394a compliance** - Enhanced features properly configured

### **Long-term Goals**  
- [ ] **DMA data transfer capability** - Actual FireWire communication working
- [ ] **Isochronous support** - Audio/video streaming capability
- [ ] **Complete OHCI 1.1 compliance** - All specification requirements met

## Development Notes

### **Build and Test Process**
1. Build: `xcodebuild -scheme ASFireWire -configuration Debug build`
2. Test logs: `log stream --predicate 'subsystem == "com.apple.kernel"' --info`
3. Commit pattern: `fix:`, `feat:`, `refactor:` prefixes

### **Key Log Patterns to Monitor**
- **Bus Reset cycles**: Look for repeated `Bus Reset (bit 17)` messages
- **PHY Register Received**: `PHY Register Received (bit 26)` indicates PHY access working
- **Cycle Inconsistent**: `Cycle Inconsistent (bit 23)` indicates timing issues
- **Self-ID Complete**: `Self-ID Complete (bit 16)` indicates successful topology discovery

### **Linux Driver Mutex Note** 
```c
/*
 * Beware! read_phy_reg(), write_phy_reg(), update_phy_reg(), and
 * read_paged_phy_reg() require the caller to hold ohci->phy_reg_mutex.
 * In other words, only use ohci_read_phy_reg() and ohci_update_phy_reg()
 * directly. Exceptions are intrinsically serialized contexts like pci_probe.
 */
```
Our PhyAccess.cpp already implements proper locking with IORecursiveLock.

---

**Last Updated**: August 26, 2025  
**Status**: AR/AT DMA contexts running ✅ — Next: AT descriptors, AR processing, Config ROM
