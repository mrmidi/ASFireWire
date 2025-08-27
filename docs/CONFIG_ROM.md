# Configuration ROM Implementation Guide

This document provides comprehensive guidance for implementing IEEE 1212-compliant Configuration ROM support in the ASFireWire OHCI driver, based on analysis of IEEE 1212-2001, OHCI 1.1 specification, Apple's IOFireWire implementation patterns, and reference implementations.

## Overview

Configuration ROM enables FireWire device discovery and capability advertisement. The ASFireWire implementation provides a minimal but spec-compliant ROM that integrates with OHCI hardware registers for autonomous CSR serving, incorporating thread-safe patterns and generation tracking from Apple's IOFireWire architecture.

## ASOHCIConfigROM Class Interface

```cpp
class ASOHCIConfigROM {
public:
    // ROM State Management (inspired by IOFireWireROMCache)
    enum ROMState {
        kROMStateSuspended,     // ROM access suspended during bus reset
        kROMStateResumed,       // ROM valid and accessible
        kROMStateInvalid        // ROM corrupted, needs rebuild
    };
    
    // ROM Construction
    bool buildMinimalROM(uint64_t eui64, uint32_t vendor_id, uint32_t capabilities);
    bool buildCompleteROM(const ROMConfig& config);
    
    // CRC Calculation (IEEE 1212 §7.3 ITU-T CRC-16)
    uint16_t calculateCRC16(const std::vector<uint32_t>& quads, size_t offset, size_t count);
    
    // Directory Management (Apple IOConfigDirectory-inspired API)
    IOReturn addRootDirectoryEntry(uint8_t key, ConfigEntryType type, uint32_t value);
    IOReturn addUnitDirectory(uint32_t specifier_id, uint32_t version);
    IOReturn addVendorDirectory(uint32_t vendor_id, const std::string& vendor_name);
    
    // Type-safe Entry Access (matching IOConfigDirectory patterns)
    IOReturn getEntryValue(uint8_t key, uint32_t& value) const;
    IOReturn getEntryType(uint8_t key, ConfigEntryType& type) const;
    IOReturn getSubdirectory(uint8_t key, ASOHCIConfigDirectory& subdir) const;
    
    // Thread-Safe Access (Apple IOFireWireROMCache patterns)
    void lock();
    void unlock();
    const void* getBytesNoCopy() const;  // Thread-safe ROM data access
    const void* getBytesNoCopy(uint32_t offset, uint32_t length) const;
    
    // Generation and State Management
    void setROMState(ROMState state, uint32_t generation = 0);
    IOReturn checkROMState(uint32_t& generation) const;
    bool hasROMChanged(const uint32_t* newBIB, uint32_t newBIBSize) const;
    
    // OHCI Hardware Integration
    uint32_t getConfigROMPhysAddr() const;  // For ConfigROMmap register (0x034)
    uint32_t getROMHeaderQuadlet() const;   // For ConfigROMheader register (0x018)
    uint32_t getBusOptionsQuadlet() const;  // For BusOptions register (0x020)
    
    // Validation and Debugging
    bool validateROM() const;
    void dumpROM(os_log_t log) const;
    IOReturn ensureCapacity(uint32_t newCapacity);
    
    // Access to ROM image
    const std::vector<uint32_t>& image() const { return _quads; }
    uint32_t getLength() const { return static_cast<uint32_t>(_quads.size() * sizeof(uint32_t)); }
    
private:
    std::vector<uint32_t> _quads;           // Big-endian ROM image
    uint32_t _physAddr{0};                  // DMA physical address
    
    // Thread safety and state (Apple IOFireWireROMCache patterns)
    mutable IORecursiveLock* _lock;         // Thread-safe access
    ROMState _state{kROMStateInvalid};      // Current ROM state
    uint32_t _generation{0};                // Bus generation when ROM was built
};

// Configuration Entry Types (IEEE 1212 Table 7, Apple IOConfigKeyType compatible)
enum class ConfigEntryType : uint8_t {
    kConfigImmediateKeyType = 0x00,     // Value stored directly in entry (IMMEDIATE)
    kConfigOffsetKeyType = 0x01,        // Offset into CSR space (CSR_OFFSET) 
    kConfigLeafKeyType = 0x02,          // Offset to leaf data structure (LEAF)
    kConfigDirectoryKeyType = 0x03,     // Offset to subdirectory (DIRECTORY)
    kInvalidConfigROMEntryType = 0xFF   // Invalid/unknown entry type
};

// Legacy compatibility aliases
using EntryType = ConfigEntryType;
static constexpr auto IMMEDIATE = ConfigEntryType::kConfigImmediateKeyType;
static constexpr auto CSR_OFFSET = ConfigEntryType::kConfigOffsetKeyType;
static constexpr auto LEAF = ConfigEntryType::kConfigLeafKeyType;
static constexpr auto DIRECTORY = ConfigEntryType::kConfigDirectoryKeyType;

// Key Definitions (IEEE 1212 Table 16 subset, Apple IOFireWireFamilyCommon compatible)
namespace ConfigROMKeys {
    constexpr uint8_t DESCRIPTOR = 0x01;
    constexpr uint8_t VENDOR = 0x03;              // MANDATORY in root directory
    constexpr uint8_t HARDWARE_VERSION = 0x04;
    constexpr uint8_t MODULE = 0x07;
    constexpr uint8_t NODE_CAPABILITIES = 0x0C;
    constexpr uint8_t EUI_64 = 0x0D;
    constexpr uint8_t UNIT = 0x11;
    constexpr uint8_t SPECIFIER_ID = 0x12;        // MANDATORY in unit directory
    constexpr uint8_t VERSION = 0x13;             // MANDATORY in unit directory
    constexpr uint8_t MODEL = 0x17;
}

// BIB Size Constants (from Apple IOFireWireROMCache)
namespace ROMConstants {
    constexpr uint32_t kROMBIBSizeMinimal = 4;    // ROM header only (4 bytes)
    constexpr uint32_t kROMBIBSizeGeneral = 20;   // ROM header + full BIB (20 bytes)
    constexpr uint32_t kROMCapacityDefault = 1024; // Default ROM buffer size (1KB)
    
    // Bus generation constants
    constexpr uint32_t kFWBIBGeneration = 0x0000f000;
    constexpr uint32_t kFWBIBGenerationPhase = 12;
}
```

## Bus Info Block Structure

The Bus Info Block is the first part of any Configuration ROM (IEEE 1212 §7.2). Apple's implementation supports both minimal and general formats:

```cpp
// Bus Info Block layout (Apple IOFireWireROMCache compatible):
struct BusInfoBlock {
    // Quadlet 0: ROM header
    uint8_t  bus_info_length;    // 3 (minimal) or 4+ (general)
    uint8_t  crc_length;         // Quadlets covered by CRC
    uint16_t crc;                // CRC-16 over following quadlets
    
    // Quadlet 1: Bus identification  
    uint32_t bus_name;           // 0x31333934 ('1394')
    
    // Quadlet 2: Node capabilities (mirrors OHCI BusOptions)
    uint32_t node_capabilities;  // Speed, max_rec, IRMC/CMC/ISC/BMC flags
    
    // Quadlets 3-4: Extended Unique Identifier (general format only)
    uint64_t eui_64;             // company_id (24 bits) + device_specific (40 bits)
};

// Format Detection (following Apple patterns)
static bool isMinimalROM(uint32_t bibSize) {
    return bibSize == ROMConstants::kROMBIBSizeMinimal;
}

static bool isGeneralROM(uint32_t bibSize) {
    return bibSize == ROMConstants::kROMBIBSizeGeneral;
}
```

### Bus Info Block Implementation (with Apple Patterns)

```cpp
bool ASOHCIConfigROM::buildMinimalROM(uint64_t eui64, uint32_t vendor_id, uint32_t capabilities) {
    lock();  // Thread-safe ROM construction
    
    _quads.clear();
    _quads.resize(8);  // 5 BIB quadlets + 3 root directory quadlets
    
    // Quadlet 0: ROM header (initially set CRC=0 for calculation)
    _quads[0] = (4 << 24) | (4 << 16) | (0 << 0);  // bus_info_length=4, crc_length=4, crc=0
    
    // Quadlet 1: Bus name
    _quads[1] = 0x31333934;  // '1394' in ASCII
    
    // Quadlet 2: Node capabilities (from OHCI BusOptions)
    _quads[2] = capabilities;
    
    // Quadlets 3-4: EUI-64
    _quads[3] = static_cast<uint32_t>(eui64 >> 32);
    _quads[4] = static_cast<uint32_t>(eui64 & 0xFFFFFFFF);
    
    // Root directory starts at quadlet 5
    // Quadlet 5: Directory header (length=2, CRC=0 initially)
    _quads[5] = (2 << 16) | (0 << 0);
    
    // Quadlet 6: Vendor_ID entry (MANDATORY)
    // Entry format: type(2) | key(6) | value(24)
    _quads[6] = (static_cast<uint32_t>(ConfigEntryType::kConfigImmediateKeyType) << 30) | 
                (ConfigROMKeys::VENDOR << 24) | (vendor_id & 0xFFFFFF);
    
    // Quadlet 7: Node_Capabilities entry (optional but recommended)
    _quads[7] = (static_cast<uint32_t>(ConfigEntryType::kConfigImmediateKeyType) << 30) | 
                (ConfigROMKeys::NODE_CAPABILITIES << 24) | capabilities;
    
    // Calculate and insert CRCs
    uint16_t bib_crc = calculateCRC16(_quads, 1, 4);  // CRC covers quadlets 1-4
    _quads[0] = (_quads[0] & 0xFFFF0000) | bib_crc;
    
    uint16_t root_crc = calculateCRC16(_quads, 6, 2);  // CRC covers quadlets 6-7
    _quads[5] = (_quads[5] & 0xFFFF0000) | root_crc;
    
    // Set ROM state to valid with generation 0 (similar to Apple's patterns)
    setROMState(kROMStateResumed, 0);
    
    unlock();
    return true;
}
```

## Thread-Safe Access Methods (Apple IOFireWireROMCache Patterns)

```cpp
void ASOHCIConfigROM::lock() {
    IORecursiveLockLock(_lock);
}

void ASOHCIConfigROM::unlock() {
    IORecursiveLockUnlock(_lock);
}

const void* ASOHCIConfigROM::getBytesNoCopy() const {
    // Thread-safe access to ROM data
    return _quads.data();
}

const void* ASOHCIConfigROM::getBytesNoCopy(uint32_t offset, uint32_t length) const {
    if (offset >= getLength() || (offset + length) > getLength()) {
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(_quads.data()) + offset;
}

// ROM State Management (Apple IOFireWireROMCache patterns)
void ASOHCIConfigROM::setROMState(ROMState state, uint32_t generation) {
    lock();
    
    // No coming back from invalid state (Apple pattern)
    if (_state != kROMStateInvalid) {
        _state = state;
    }
    
    if (_state == kROMStateResumed) {
        _generation = generation;
    }
    
    unlock();
}

IOReturn ASOHCIConfigROM::checkROMState(uint32_t& generation) const {
    IOReturn status = kIOReturnSuccess;
    
    // Use const_cast for thread safety (matching Apple patterns)
    const_cast<ASOHCIConfigROM*>(this)->lock();
    
    if (_state == kROMStateInvalid) {
        status = kIOFireWireConfigROMInvalid;
    } else if (_state == kROMStateResumed) {
        status = kIOReturnSuccess;
    }
    
    generation = _generation;
    const_cast<ASOHCIConfigROM*>(this)->unlock();
    
    return status;
}

// ROM Change Detection (Apple hasROMChanged pattern)
bool ASOHCIConfigROM::hasROMChanged(const uint32_t* newBIB, uint32_t newBIBSize) const {
    bool rom_changed = false;
    
    lock();
    
    // Format change detection
    if (newBIBSize > getLength()) {
        rom_changed = true;  // Minimal -> General
    }
    
    if (newBIBSize < ROMConstants::kROMBIBSizeGeneral && 
        getLength() >= ROMConstants::kROMBIBSizeGeneral) {
        rom_changed = true;  // General -> Minimal
    }
    
    // Content comparison for general ROM
    if (newBIBSize == ROMConstants::kROMBIBSizeGeneral) {
        if (memcmp(newBIB, getBytesNoCopy(), newBIBSize) != 0) {
            rom_changed = true;
        }
        
        // Generation 0 devices may have slow unit publication (Apple pattern)
        uint32_t bib_quad = OSSwapBigToHostInt32(newBIB[2]);
        uint32_t romGeneration = (bib_quad & ROMConstants::kFWBIBGeneration) >> ROMConstants::kFWBIBGenerationPhase;
        if (romGeneration == 0) {
            rom_changed = true;  // Always reconsider generation 0
        }
    }
    
    // Don't recover from invalid state
    if (_state == kROMStateInvalid) {
        rom_changed = true;
    }
    
    const_cast<ASOHCIConfigROM*>(this)->unlock();
    return rom_changed;
}
```

## CRC-16 Calculation

IEEE 1212 §7.3 specifies ITU-T CRC-16 calculation:

```cpp
uint16_t ASOHCIConfigROM::calculateCRC16(const std::vector<uint32_t>& quads, size_t offset, size_t count) {
    uint16_t crc = 0;
    
    for (size_t i = offset; i < offset + count; i++) {
        // Process each quadlet as two doublets (big-endian)
        uint16_t doublet1 = static_cast<uint16_t>(quads[i] >> 16);
        uint16_t doublet2 = static_cast<uint16_t>(quads[i] & 0xFFFF);
        
        crc = crc16_update(crc, doublet1);
        crc = crc16_update(crc, doublet2);
    }
    
    return crc;
}

// ITU-T CRC-16 update function (IEEE 1212 Table 4)
uint16_t ASOHCIConfigROM::crc16_update(uint16_t crc, uint16_t data) {
    uint16_t e = crc ^ data;
    
    // Apply ITU-T CRC-16 polynomial transformation
    uint16_t new_crc = 0;
    new_crc |= ((e >> 12) ^ (e >> 11) ^ (e >> 8) ^ (e >> 4)) & 0x0001;          // C15
    new_crc |= ((e >> 13) ^ (e >> 12) ^ (e >> 9) ^ (e >> 5)) & 0x0002;          // C14
    new_crc |= ((e >> 14) ^ (e >> 13) ^ (e >> 10) ^ (e >> 6)) & 0x0004;         // C13
    new_crc |= ((e >> 15) ^ (e >> 14) ^ (e >> 11) ^ (e >> 7)) & 0x0008;         // C12
    new_crc |= ((e >> 0) ^ (e >> 15) ^ (e >> 12) ^ (e >> 8)) & 0x0010;          // C11
    new_crc |= ((e >> 1) ^ (e >> 0) ^ (e >> 13) ^ (e >> 9)) & 0x0020;           // C10
    // ... (complete implementation per IEEE 1212 Table 4)
    
    return new_crc;
}
```

## OHCI Hardware Integration

### Register Programming Sequence

```cpp
// In ASOHCI::Start() after DMA setup:
bool ASOHCI::setupConfigROM() {
    // 1. Build minimal ROM
    if (!_configROM.buildMinimalROM(_deviceEUI64, _vendorID, _nodeCapabilities)) {
        os_log_error(_log, "ASOHCI: Failed to build Configuration ROM");
        return false;
    }
    
    // 2. Allocate and map ROM buffer (1KB, 32-bit DMA)
    auto rom_buffer = IOBufferMemoryDescriptor::withBytes(
        _configROM.image().data(), 
        _configROM.image().size() * sizeof(uint32_t), 
        kIODirectionOut
    );
    
    auto dma_cmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 32, 0, nullptr, 0, 1, nullptr, nullptr
    );
    
    IODMACommand::Segment64 segment;
    UInt32 segmentCount = 1;
    dma_cmd->gen64IOVMSegments(&segment, &segmentCount);
    
    uint32_t rom_phys_addr = static_cast<uint32_t>(segment.fIOVMAddr);
    
    // 3. Program OHCI registers
    // ConfigROMheader = 0 (Linux workaround per ohci.c:2549)
    pci->MemoryWrite32(_barIndex, kOHCI_ConfigROMhdr, 0);
    
    // BusOptions from ROM quadlet 2
    pci->MemoryWrite32(_barIndex, kOHCI_BusOptions, _configROM.getBusOptionsQuadlet());
    
    // ConfigROMmap points to ROM buffer
    pci->MemoryWrite32(_barIndex, kOHCI_ConfigROMmap, rom_phys_addr);
    
    os_log_info(_log, "ASOHCI: ConfigROM mapped @ 0x%08x", rom_phys_addr);
    return true;
}

// Enable BIBimageValid after link enable (in link sequence)
void ASOHCI::enableConfigROMBlocks() {
    // Set BIBimageValid to enable block read responses
    pci->MemoryWrite32(_barIndex, kOHCI_HCControlSet, kOHCIHCControl_BIBimageValid);
    os_log_info(_log, "ASOHCI: BIBimageValid set - block ROM reads enabled");
}
```

### Atomic ROM Updates (OHCI §5.5.6)

```cpp
void ASOHCI::updateConfigROM(const ASOHCIConfigROM& newROM) {
    // 1. Prepare new ROM buffer
    // 2. Write to ConfigROMmap (actually updates ConfigROMmapNext shadow register)
    pci->MemoryWrite32(_barIndex, kOHCI_ConfigROMmap, newROM.getConfigROMPhysAddr());
    
    // 3. Force bus reset to trigger atomic update
    pci->MemoryWrite32(_barIndex, kOHCI_PhyReqFilterHiSet, kOHCIPhyReqFilterHi_BusReset);
    
    // Hardware will:
    // - Update ConfigROMmap from ConfigROMmapNext after bus reset
    // - Reload ConfigROMheader and BusOptions from new ROM location
    // - Serve updated ROM to 1394 bus requests
}
```

## Directory Entry Format

IEEE 1212 directory entries are 32-bit values with this format:

```
Bits 31-30: Entry Type (IMMEDIATE=0, CSR_OFFSET=1, LEAF=2, DIRECTORY=3)
Bits 29-24: Key ID (see IEEE 1212 Table 16)
Bits 23-0:  Value (immediate data or quadlet offset)
```

### Adding Directory Entries

```cpp
void ASOHCIConfigROM::addRootDirectoryEntry(uint8_t key, EntryType type, uint32_t value) {
    uint32_t entry = (static_cast<uint32_t>(type) << 30) | 
                     ((key & 0x3F) << 24) | 
                     (value & 0xFFFFFF);
    
    // Insert into root directory and update length/CRC
    size_t root_dir_start = 5;  // After 5-quadlet BIB
    _quads.insert(_quads.begin() + root_dir_start + 1, entry);
    
    // Update directory length
    uint32_t length = (_quads.size() - root_dir_start - 1);
    _quads[root_dir_start] = (length << 16) | 0;  // CRC will be recalculated
    
    // Recalculate directory CRC
    uint16_t crc = calculateCRC16(_quads, root_dir_start + 1, length);
    _quads[root_dir_start] = (_quads[root_dir_start] & 0xFFFF0000) | crc;
}
```

## Validation and Debugging

```cpp
bool ASOHCIConfigROM::validateROM() const {
    if (_quads.size() < 8) return false;  // Minimum: 5 BIB + 3 root dir
    
    // Validate Bus Info Block
    uint8_t bus_info_length = static_cast<uint8_t>(_quads[0] >> 24);
    uint8_t crc_length = static_cast<uint8_t>(_quads[0] >> 16);
    uint16_t stored_crc = static_cast<uint16_t>(_quads[0]);
    
    if (bus_info_length < 3 || crc_length < bus_info_length) return false;
    
    // Verify BIB CRC
    uint16_t calculated_crc = calculateCRC16(_quads, 1, crc_length);
    if (calculated_crc != stored_crc) return false;
    
    // Verify bus_name = '1394'
    if (_quads[1] != 0x31333934) return false;
    
    // Validate root directory
    size_t root_offset = bus_info_length + 1;
    if (root_offset >= _quads.size()) return false;
    
    uint16_t root_length = static_cast<uint16_t>(_quads[root_offset] >> 16);
    uint16_t root_crc = static_cast<uint16_t>(_quads[root_offset]);
    
    uint16_t calculated_root_crc = calculateCRC16(_quads, root_offset + 1, root_length);
    if (calculated_root_crc != root_crc) return false;
    
    // Verify mandatory Vendor_ID entry exists
    bool has_vendor_id = false;
    for (size_t i = root_offset + 1; i < root_offset + 1 + root_length; i++) {
        uint8_t key = static_cast<uint8_t>(_quads[i] >> 24) & 0x3F;
        if (key == ConfigROMKeys::VENDOR) {
            has_vendor_id = true;
            break;
        }
    }
    
    return has_vendor_id;
}

void ASOHCIConfigROM::dumpROM(os_log_t log) const {
    os_log_info(log, "ConfigROM dump (%zu quadlets):", _quads.size());
    
    // Dump Bus Info Block
    os_log_info(log, "  BIB[0]: length=%d, crc_len=%d, crc=0x%04x", 
                static_cast<uint8_t>(_quads[0] >> 24),
                static_cast<uint8_t>(_quads[0] >> 16),
                static_cast<uint16_t>(_quads[0]));
    os_log_info(log, "  BIB[1]: bus_name=0x%08x", _quads[1]);
    os_log_info(log, "  BIB[2]: capabilities=0x%08x", _quads[2]);
    os_log_info(log, "  BIB[3-4]: EUI-64=0x%08x%08x", _quads[3], _quads[4]);
    
    // Dump Root Directory
    size_t root_offset = 5;
    uint16_t root_length = static_cast<uint16_t>(_quads[root_offset] >> 16);
    os_log_info(log, "  Root[%zu]: length=%d, crc=0x%04x", root_offset, root_length, 
                static_cast<uint16_t>(_quads[root_offset]));
    
    for (size_t i = 1; i <= root_length; i++) {
        uint32_t entry = _quads[root_offset + i];
        uint8_t type = static_cast<uint8_t>(entry >> 30);
        uint8_t key = static_cast<uint8_t>(entry >> 24) & 0x3F;
        uint32_t value = entry & 0xFFFFFF;
        
        const char* type_name = (type == 0) ? "IMM" : (type == 1) ? "CSR" : 
                               (type == 2) ? "LEAF" : "DIR";
        os_log_info(log, "    Entry[%zu]: %s key=0x%02x value=0x%06x", 
                    root_offset + i, type_name, key, value);
    }
}
```

## Integration with ASFireWire

### OHCIConstants.hpp additions

```cpp
// ConfigROM OHCI register offsets
static constexpr uint32_t kOHCI_ConfigROMhdr = 0x018;
static constexpr uint32_t kOHCI_BusID = 0x01C;        
static constexpr uint32_t kOHCI_BusOptions = 0x020;
static constexpr uint32_t kOHCI_ConfigROMmap = 0x034;

// HCControl bits for ConfigROM
static constexpr uint32_t kOHCIHCControl_BIBimageValid = (1 << 18);

// Error codes (Apple IOFireWire compatibility)
static constexpr IOReturn kIOFireWireConfigROMInvalid = iokit_fw_err(0x1000);
```

### Usage in ASOHCI.cpp (with Apple Patterns)

```cpp
// In ASOHCI.h - add instance variable:
class ASOHCI {
    // ...
private:
    ASOHCIConfigROM* _configROM{nullptr};
    uint32_t _busGeneration{0};
};

// In Start() method after PCI setup:
_configROM = new ASOHCIConfigROM();
if (!_configROM) {
    os_log_error(_log, "ASOHCI: Failed to allocate ConfigROM");
    return kIOReturnNoMemory;
}

if (!setupConfigROM()) {
    os_log_error(_log, "ASOHCI: ConfigROM setup failed");
    return kIOReturnError;
}

// In link enable sequence:
enableConfigROMBlocks();

// In bus reset handler - suspend ROM during reset
void ASOHCI::handleBusReset() {
    if (_configROM) {
        _configROM->setROMState(ASOHCIConfigROM::kROMStateSuspended);
    }
    // ... handle bus reset
}

// After Self-ID complete - resume ROM with new generation
void ASOHCI::handleSelfIDComplete() {
    // ... process Self-ID
    _busGeneration++;
    if (_configROM) {
        _configROM->setROMState(ASOHCIConfigROM::kROMStateResumed, _busGeneration);
    }
}

// In Stop() method:
if (_configROM) {
    delete _configROM;
    _configROM = nullptr;
}
```

## Reference Alignment

This implementation aligns with:

- **IEEE 1212-2001**: Full compliance with ROM format, CRC calculation, and directory structure requirements
- **OHCI 1.1 §5.5**: Proper integration with autonomous CSR resources and register programming
- **Apple IOFireWire Architecture**: Thread-safe ROM caching, generation tracking, and state management patterns from IOFireWireROMCache
- **Apple IOKit Patterns**: IOReturn error codes, IORecursiveLock threading, OSObject conventions
- **Python Parser**: Directory traversal patterns match `config_rom_lexer.py` and `root_directory_parser.py` logic
- **Linux ohci.c**: ConfigROM setup sequence follows established patterns (lines 2537-2558)

## Testing and Validation

Validate the implementation by:

1. **CRC Verification**: Compare calculated CRC values with reference implementations
2. **Parser Compatibility**: Verify Python parser can correctly parse generated ROM
3. **OHCI Integration**: Confirm proper register programming and BIBimageValid operation
4. **Thread Safety**: Validate concurrent access patterns and generation tracking
5. **State Management**: Test ROM state transitions across bus resets and errors
6. **Apple Compatibility**: Ensure ROM format matches expectations of macOS FireWire stack
7. **Bus Compliance**: Test ROM serving to external 1394 devices and enumeration software

## Advanced Features

### ROM Iterator Support (Apple IOConfigDirectory Pattern)

```cpp
// Future enhancement: Directory traversal iterator
class ASOHCIConfigDirectoryIterator {
public:
    IOReturn init(const ASOHCIConfigROM& rom, uint8_t key_filter = 0xFF);
    ASOHCIConfigEntry* getNextEntry();
    void reset();
    bool isValid() const;
};

// Usage example:
ASOHCIConfigDirectoryIterator iter;
iter.init(*_configROM, ConfigROMKeys::UNIT);  // Find all unit directories
while (auto entry = iter.getNextEntry()) {
    // Process unit directory entries
}
```

### Enhanced Error Recovery

```cpp
// ROM corruption detection and recovery
IOReturn ASOHCIConfigROM::validateAndRecover() {
    lock();
    
    if (!validateROM()) {
        os_log_error(ASOHCITraceLog(), "ConfigROM: Corruption detected, rebuilding");
        setROMState(kROMStateInvalid);
        
        // Attempt to rebuild from cached parameters
        if (!buildMinimalROM(_cachedEUI64, _cachedVendorID, _cachedCapabilities)) {
            unlock();
            return kIOReturnError;
        }
    }
    
    unlock();
    return kIOReturnSuccess;
}
```

This ConfigROM implementation provides a robust, thread-safe foundation for proper FireWire device advertisement and discovery within the ASFireWire OHCI driver, incorporating proven patterns from Apple's IOFireWire architecture while maintaining full IEEE 1212 compliance.