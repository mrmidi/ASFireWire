#include "HardwareInterface.hpp"

#include <algorithm>

#include "Logging.hpp"

#ifndef ASFW_HOST_TEST
#include <DriverKit/IOService.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSAction.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>
#else
#include <chrono>
#include <thread>
#endif

// RAII guard for IOLock (DriverKit C API)
// Matches Linux phy_reg_mutex discipline (ohci.c read_phy_reg/write_phy_reg)
namespace {
struct IOLockGuard {
    IOLock* lock;
    explicit IOLockGuard(IOLock* l) : lock(l) { if (lock) IOLockLock(lock); }
    ~IOLockGuard() { if (lock) IOLockUnlock(lock); }
    IOLockGuard(const IOLockGuard&) = delete;
    IOLockGuard& operator=(const IOLockGuard&) = delete;
};
}

// MEMORY SETUP VERIFICATION STATUS:
// ✅ PCI Command Register: Sets bus master + memory space bits (matches Linux pci_set_master)
// ✅ BAR Configuration: Uses BAR 0 for OHCI registers (matches Linux pcim_iomap_region)
// ✅ Register Access: 32-bit quadlet access on boundaries (complies with OHCI §4.4)
// ✅ DMA Setup: 32-bit address space, proper DriverKit APIs
// ✅ BAR Validation: Enforces size (≥2048) and memory-space type requirements
// ✅ DMA Alignment: Configurable alignment (default 64-byte), supports 16-byte for descriptors
// ✅ PCI Write Verification: Reads back command register to confirm bus master + memory enable
// ✅ BAR Index Validation: Confirms returned memory index matches requested BAR

namespace ASFW::Driver {

namespace {
constexpr uint8_t kDefaultBAR = 0;
constexpr uint64_t kDefaultDMAMaxAddressBits = 32;
#ifndef ASFW_HOST_TEST
constexpr uint16_t kRequiredCommandBits = kIOPCICommandBusMaster | kIOPCICommandMemorySpace;
#else
constexpr uint16_t kRequiredCommandBits = 0;
#endif

// PHY config packet bit-field helpers (IEEE 1394-2008 §16.3.3)
constexpr uint32_t kPhyPacketIdentifierPhyConfig = 0x02u;
constexpr uint32_t kPhyConfigPacketIdBits = kPhyPacketIdentifierPhyConfig << 24;
constexpr uint32_t kPhyConfigForceRootMask = 0x00800000u;
constexpr uint32_t kPhyConfigGapCountMask = 0x003FFFFFu;
}

HardwareInterface::HardwareInterface() {
    // Allocate PHY register access mutex per Linux phy_reg_mutex (ohci.c)
    phyLock_ = IOLockAlloc();
}

HardwareInterface::~HardwareInterface() {
    if (phyLock_) {
        IOLockFree(phyLock_);
        phyLock_ = nullptr;
    }
    Detach();
}

kern_return_t HardwareInterface::Attach(IOService* owner, IOService* provider) {
    if (device_) {
        return kIOReturnSuccess;
    }

    auto pci = OSSharedPtr(OSDynamicCast(IOPCIDevice, provider), OSRetain);
    if (!pci) {
        return kIOReturnBadArgument;
    }

    kern_return_t kr = pci->Open(owner);
    if (kr != kIOReturnSuccess) {
        return kr;
    }

#ifndef ASFW_HOST_TEST
    // Read PCI vendor/device ID for quirk detection (before command setup)
    uint16_t vendorId = 0, deviceId = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vendorId);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &deviceId);
    
    // Detect Agere/LSI chipset (reports invalid eventCode 0x10 in AT completion)
    quirk_agere_lsi_ = (vendorId == 0x11c1 && (deviceId == 0x5901 || deviceId == 0x5900));
    if (quirk_agere_lsi_) {
        ASFW_LOG(Hardware, "⚠️  Agere/LSI chipset detected (vendor=0x%04x device=0x%04x) - enabling eventCode 0x10 workaround",
                 vendorId, deviceId);
    }
    
    uint16_t command = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &command);

    const uint16_t desired = command | kRequiredCommandBits;
    if (desired != command) {
        pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, desired);
    }

    uint16_t commandVerify = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &commandVerify);
    if ((commandVerify & kRequiredCommandBits) != kRequiredCommandBits) {
        pci->Close(owner);
        return kIOReturnNotReady;
    }
#endif

    constexpr uint64_t kMinRegisterBytes = 2048;
    uint64_t barSize = 0;
    uint8_t barType = 0;
    uint8_t memoryIndex = 0;
    kr = pci->GetBARInfo(kDefaultBAR, &memoryIndex, &barSize, &barType);
    if (kr != kIOReturnSuccess) {
        pci->Close(owner);
        return kr;
    }

    const bool barIsMemory = (barType == kPCIBARTypeM32 ||
                              barType == kPCIBARTypeM32PF ||
                              barType == kPCIBARTypeM64 ||
                              barType == kPCIBARTypeM64PF);
    if (!barIsMemory) {
        pci->Close(owner);
        return kIOReturnUnsupported;
    }

    if (barSize < kMinRegisterBytes) {
        pci->Close(owner);
        return kIOReturnNoResources;
    }

    if (memoryIndex != kDefaultBAR) {
        pci->Close(owner);
        return kIOReturnUnsupported;
    }

    device_ = std::move(pci);
    owner_ = owner;
    barIndex_ = memoryIndex;
    barSize_ = barSize;
    barType_ = barType;
    return kIOReturnSuccess;
}

void HardwareInterface::Detach() {
    if (device_) {
        if (owner_) {
            device_->Close(owner_);
        }
        device_.reset();
    }
    owner_ = nullptr;
    barSize_ = 0;
    barType_ = 0;
}

uint32_t HardwareInterface::Read(Register32 reg) const noexcept {
    if (!device_) {
        return 0;
    }
    // TODO: OHCI Spec §4.4 - Ensure quadlet boundary access
    // - All register accesses must be 32-bit on quadlet (4-byte) boundaries
    // - Register32 enum values should all be multiples of 4
    // - Current implementation assumes Register32 enum is correctly defined
    // - Linux uses reg_read/write macros that ensure 32-bit access
    uint32_t value = 0;
    device_->MemoryRead32(barIndex_, static_cast<uint64_t>(reg), &value);
    return value;
}

void HardwareInterface::Write(Register32 reg, uint32_t value) noexcept {
    if (!device_) {
        return;
    }
    // TODO: OHCI Spec §4.4 - Ensure quadlet boundary access
    // - All register writes must be 32-bit on quadlet boundaries
    // - No 8-bit or 16-bit access allowed to OHCI registers
    // - Current implementation correctly uses MemoryWrite32
    device_->MemoryWrite32(barIndex_, static_cast<uint64_t>(reg), value);
}

void HardwareInterface::WriteAndFlush(Register32 reg, uint32_t value) {
    Write(reg, value);
    FlushPostedWrites();
}

void HardwareInterface::SetInterruptMask(uint32_t mask, bool enable) {
    if (!device_) {
        return;
    }
    Register32 target = enable ? Register32::kIntMaskSet : Register32::kIntMaskClear;
    device_->MemoryWrite32(barIndex_, static_cast<uint64_t>(target), mask);
    FlushPostedWrites();
}

void HardwareInterface::SetLinkControlBits(uint32_t bits) {
    WriteAndFlush(Register32::kLinkControlSet, bits);
}

void HardwareInterface::ClearLinkControlBits(uint32_t bits) {
    WriteAndFlush(Register32::kLinkControlClear, bits);
}

void HardwareInterface::ClearIntEvents(uint32_t mask) {
    if (!mask) {
        return;
    }
    WriteAndFlush(Register32::kIntEventClear, mask);
}

void HardwareInterface::ClearIsoXmitEvents(uint32_t mask) {
    if (!mask) {
        return;
    }
    WriteAndFlush(Register32::kIsoXmitIntEventClear, mask);
}

void HardwareInterface::ClearIsoRecvEvents(uint32_t mask) {
    if (!mask) {
        return;
    }
    WriteAndFlush(Register32::kIsoRecvIntEventClear, mask);
}

InterruptSnapshot HardwareInterface::CaptureInterruptSnapshot(uint64_t timestamp) const noexcept {
    InterruptSnapshot snapshot{};
    snapshot.timestamp = timestamp;
    if (!device_) {
        return snapshot;
    }

    device_->MemoryRead32(barIndex_, static_cast<uint64_t>(Register32::kIntEvent), &snapshot.intEvent);
    // NOTE: IntMaskSet/Clear are write-only strobes per OHCI §5.7 - cannot read enabled mask from hardware.
    // InterruptManager maintains a shadow mask. Caller should query InterruptManager::EnabledMask() instead.
    // Setting intMask to 0 here to avoid confusion; real mask comes from InterruptManager shadow.
    snapshot.intMask = 0;
    device_->MemoryRead32(barIndex_, static_cast<uint64_t>(Register32::kIsoXmitEvent), &snapshot.isoXmitEvent);
    device_->MemoryRead32(barIndex_, static_cast<uint64_t>(Register32::kIsoRecvEvent), &snapshot.isoRecvEvent);
    return snapshot;
}

bool HardwareInterface::SendPhyConfig(std::optional<uint8_t> gapCount,
                                      std::optional<uint8_t> forceRootPhyId) {
    if (!device_) {
        return false;
    }

    const uint8_t rootId = static_cast<uint8_t>(forceRootPhyId.value_or(0) & 0x0Fu);
    const uint32_t gap = static_cast<uint32_t>(std::min<uint32_t>(gapCount.value_or(0), kPhyConfigGapCountMask));

    uint32_t quad = kPhyConfigPacketIdBits;
    quad |= static_cast<uint32_t>(rootId) << 24;
    if (forceRootPhyId.has_value()) {
        quad |= kPhyConfigForceRootMask;
    }

    if (gapCount.has_value()) {
        quad |= gap;
        // TODO(ASFW-BusReset): Evaluate gap-count optimisation heuristics; keep cleared for now.
    }

    ASFW_LOG(Hardware,
           "TODO(ASFW-BusReset): Queue PHY CONFIG packet (quad=0x%08x) via AT context",
           quad);

    // TODO(ASFW-BusReset): Issue quadlet through AT request context (tCode=PHY) once
    // the async transport scaffolding is in place.
    return false;
}

bool HardwareInterface::InitiateBusReset(bool shortReset) {
    // Serialize PHY access (mutex acquired inside WritePhyRegister)
    // IEEE 1394a: PHY register 1 initiates bus reset
    // bit 6 (IBR) = short reset; bit 7 = long reset on many PHYs
    // Per Linux ohci.c: uses update_phy_reg(1, 0, 0x40) for reset
    const uint8_t data = shortReset ? 0x40 : 0xC0;
    return WritePhyRegister(/*addr=*/1, data);
}

std::optional<uint8_t> HardwareInterface::ReadPhyRegister(uint8_t address) {
    IOLockGuard guard(phyLock_);
    return ReadPhyRegisterUnlocked(address);
}

std::optional<uint8_t> HardwareInterface::ReadPhyRegisterUnlocked(uint8_t address) {
    // Assumes phyLock_ is already held by caller
    // Per OHCI §5.12 / Fig 5-21: PhyControl register read operation
    // rdReg    = bit 15 (0x8000) - set to initiate, hardware clears when done
    // regAddr  = bits 13:8 (PHY register address)
    // rdData   = bits 23:16 (data returned from PHY)
    // rdDone   = bit 31 (0x80000000) - set by hardware when read completes
    //
    // Per Linux read_phy_reg() (ohci.c line 639):
    //   reg_write(ohci, OHCI1394_PhyControl, OHCI1394_PhyControl_Read(addr));
    // where OHCI1394_PhyControl_Read(a) = ((a) << 8) | 0x00008000

    const uint32_t phyControl = (static_cast<uint32_t>(address) << 8) | 0x8000u;  // rdReg bit

    Write(Register32::kPhyControl, phyControl);
    FlushPostedWrites();

    ASFW_LOG_PHY("[PHY] Read reg %u: wrote PhyControl=0x%08x", address, phyControl);

    // Poll for rdDone bit (bit 31) with timeout
    // Linux uses 3 immediate tries + 100 tries with 1ms sleep (total 103)
    constexpr int kImmediateTries = 3;
    constexpr int kTotalTries = 103;

    for (int i = 0; i < kTotalTries; i++) {
        const uint32_t val = Read(Register32::kPhyControl);

        // Check for card ejection (all bits set)
        if (val == 0xFFFFFFFF) {
            ASFW_LOG(Hardware, "[PHY] Read reg %u failed - card ejected", address);
            return std::nullopt;
        }

        // Check for rdDone (bit 31)
        if (val & 0x80000000u) {
            // Extract data from bits 23:16 (rdData field)
            const uint8_t data = static_cast<uint8_t>((val >> 16) & 0xFF);
            ASFW_LOG_PHY("[PHY] Read reg %u success (iter %d): rdData=0x%02x",
                        address, i, data);
            return data;
        }

        // Log slow polling for diagnostics
        if (i == kImmediateTries) {
            ASFW_LOG_PHY("[PHY] Read reg %u: rdDone not set after %d fast polls, entering slow poll (val=0x%08x)",
                         address, kImmediateTries, val);
        }

        // Sleep after immediate tries (matches Linux behavior)
        if (i >= kImmediateTries) {
            IOSleep(1);  // 1ms sleep
        }
    }

    const uint32_t finalVal = Read(Register32::kPhyControl);
    ASFW_LOG(Hardware, "[PHY] Read reg %u TIMEOUT after %d iterations (final PhyControl=0x%08x)", 
             address, kTotalTries, finalVal);
    return std::nullopt;
}

bool HardwareInterface::WritePhyRegister(uint8_t address, uint8_t value) {
    IOLockGuard guard(phyLock_);
    return WritePhyRegisterUnlocked(address, value);
}

bool HardwareInterface::WritePhyRegisterUnlocked(uint8_t address, uint8_t value) {
    // Assumes phyLock_ is already held by caller
    // OHCI §5.12 / Fig. 5-21: PhyControl register write operation
    // wrReg    = bit 14 (0x4000) - set to initiate, hardware clears when done
    // regAddr  = bits 15:8 (PHY register address)
    // wrData   = bits 7:0 (data to write)
    //
    // Per Linux write_phy_reg() (ohci.c line 665):
    //   reg_write(ohci, OHCI1394_PhyControl, OHCI1394_PhyControl_Write(addr, val));
    // where OHCI1394_PhyControl_Write(a,d) = ((a) << 8) | (d) | 0x00004000

    const uint32_t phyControl = (static_cast<uint32_t>(address) << 8) |
                                static_cast<uint32_t>(value) | 0x4000u;  // wrReg bit

    Write(Register32::kPhyControl, phyControl);
    FlushPostedWrites();

    // Poll for wrReg bit to clear (bit 14) with timeout
    // Linux uses 3 immediate tries + 100 tries with 1ms sleep (ohci.c line 666-672)
    constexpr int kImmediateTries = 3;
    constexpr int kTotalTries = 103;

    for (int i = 0; i < kTotalTries; i++) {
        const uint32_t val = Read(Register32::kPhyControl);

        // Check for card ejection
        if (val == 0xFFFFFFFF) {
            ASFW_LOG(Hardware, "PHY write failed - card ejected");
            return false;
        }

        // Check if wrReg cleared (bit 14) - hardware clears when transaction completes
        if ((val & 0x4000u) == 0) {
            ASFW_LOG_PHY("PHY[%u] write OK: 0x%02x", address, value);
            return true;
        }

        // Sleep after immediate tries
        if (i >= kImmediateTries) {
            IOSleep(1);  // 1ms sleep
        }
    }

    ASFW_LOG(Hardware, "PHY[%u] write timeout (wrReg still set): 0x%02x", address, value);
    return false;
}

bool HardwareInterface::UpdatePhyRegister(uint8_t address, uint8_t clearBits, uint8_t setBits) {
    // Serialize PHY access per Linux phy_reg_mutex (ohci.c update_phy_reg line 684)
    IOLockGuard guard(phyLock_);

    // Per Linux ohci.c update_phy_reg() at line 684-699
    // Read-modify-write PHY register

    ASFW_LOG_PHY("Updating PHY[%u]: clear=0x%02x set=0x%02x", address, clearBits, setBits);

    // Read current value (use unlocked version - we already hold phyLock_)
    const auto currentOpt = ReadPhyRegisterUnlocked(address);
    if (!currentOpt.has_value()) {
        ASFW_LOG(Hardware, "PHY register %u update failed - read failed", address);
        return false;
    }

    uint8_t current = currentOpt.value();

    // Per Linux: PHY register 5 has interrupt status bits that are cleared by writing 1
    // Avoid clearing them unless explicitly requested in setBits
    if (address == 5) {
        constexpr uint8_t kPhyIntStatusBits = 0x3C;  // bits 2-5 per IEEE 1394
        clearBits |= kPhyIntStatusBits;
    }

    // Apply modifications
    const uint8_t newValue = (current & ~clearBits) | setBits;

    ASFW_LOG_PHY("PHY register %u: 0x%02x → 0x%02x", address, current, newValue);

    // Write new value (use unlocked version - we already hold phyLock_)
    return WritePhyRegisterUnlocked(address, newValue);
}

bool HardwareInterface::ReadIntEvent(uint32_t& value) {
    if (!device_) {
        return false;
    }
    device_->MemoryRead32(barIndex_, static_cast<uint64_t>(Register32::kIntEvent), &value);
    return true;
}

void HardwareInterface::AckIntEvent(uint32_t bits) {
    if (!device_) {
        return;
    }
    device_->MemoryWrite32(barIndex_, static_cast<uint64_t>(Register32::kIntEventClear), bits);
    FlushPostedWrites();
}

void HardwareInterface::IntMaskSet(uint32_t bits) {
    if (!device_) {
        return;
    }
    device_->MemoryWrite32(barIndex_, static_cast<uint64_t>(Register32::kIntMaskSet), bits);
    FlushPostedWrites();
}

void HardwareInterface::IntMaskClear(uint32_t bits) {
    if (!device_) {
        return;
    }
    device_->MemoryWrite32(barIndex_, static_cast<uint64_t>(Register32::kIntMaskClear), bits);
    FlushPostedWrites();
}

std::optional<HardwareInterface::DMABuffer> HardwareInterface::AllocateDMA(size_t length, uint64_t options, size_t alignment) {
    // OHCI Spec §1.7 - DMA buffer alignment requirements:
    // - Config ROM: 1KB alignment (1024 bytes)
    // - DMA descriptors: 16-byte alignment (OHCI Table 7-3)
    // - Default: 64-byte alignment (cache line friendly)
    //
    // Alignment validation per OHCI Table 7-3:
    // - Descriptor blocks MUST be 16-byte aligned
    // - branchAddress field bits [3:0] are Z value (not address bits)
    // - Physical address bits [3:0] must be 0
    //
    // CRITICAL: OHCI supports only 32-bit DMA addressing (max 4GB physical addresses)
    // Use IODMACommand with maxAddressBits=32 to get IOMMU-mapped addresses

    if (!device_) {
        ASFW_LOG(Hardware, "DMA allocation failed - no PCI device");
        return std::nullopt;
    }

    // Validate direction includes both read and write (bidirectional)
    // CRITICAL: Buffer must be CPU-writable for memset/descriptor initialization
    if ((options & (kIOMemoryDirectionOut | kIOMemoryDirectionIn)) != (kIOMemoryDirectionOut | kIOMemoryDirectionIn)) {
        ASFW_LOG(Hardware, "⚠️  AllocateDMA: options=0x%llx may not be bidirectional - ensure kIOMemoryDirectionInOut", options);
    }

    IOBufferMemoryDescriptor* buffer = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(options, length, alignment, &buffer);
    if (kr != kIOReturnSuccess || buffer == nullptr) {
        ASFW_LOG(Hardware, "IOBufferMemoryDescriptor::Create failed: 0x%08x", kr);
        return std::nullopt;
    }

    buffer->SetLength(length);

    // Create IODMACommand with 32-bit addressing constraint
    IODMACommandSpecification spec{};
    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = kDefaultDMAMaxAddressBits;

    IODMACommand* dmaCmd = nullptr;
    kr = IODMACommand::Create(device_.get(), kIODMACommandCreateNoOptions, &spec, &dmaCmd);
    if (kr != kIOReturnSuccess || dmaCmd == nullptr) {
        ASFW_LOG(Hardware, "IODMACommand::Create failed: 0x%08x", kr);
        buffer->release();
        return std::nullopt;
    }
    OSSharedPtr<IODMACommand> command(dmaCmd, OSNoRetain);

    // Prepare the buffer for DMA - this returns IOMMU-mapped physical addresses
    IOAddressSegment segments[32];
    uint32_t segmentCount = 32;
    uint64_t flags = 0;

    kr = command->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                buffer,
                                0,           // offset
                                length,      // length
                                &flags,
                                &segmentCount,
                                segments);

    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Hardware,
               "IODMACommand::PrepareForDMA failed: 0x%08x - IOMMU mapping failed",
               kr);
        command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        buffer->release();
        return std::nullopt;
    }

    // Verify we got exactly one segment and it's within 32-bit range
    if (segmentCount == 0) {
        ASFW_LOG(Hardware, "IODMACommand::PrepareForDMA returned zero segments");
        command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        buffer->release();
        return std::nullopt;
    }

    if (segmentCount > 1) {
        ASFW_LOG(Hardware,
               "WARNING: DMA buffer fragmented into %u segments - using first segment only",
               segmentCount);
    }

    uint64_t mappedAddress = segments[0].address;
    if (mappedAddress > 0xFFFFFFFFULL) {
        ASFW_LOG(Hardware,
               "DMA segment paddr=0x%llx exceeds 32-bit range - IOMMU failed to map below 4GB",
               mappedAddress);
        command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        buffer->release();
        return std::nullopt;
    }

    // Verify alignment matches request (critical for OHCI descriptor/ROM requirements)
    if ((mappedAddress & (alignment - 1)) != 0) {
        ASFW_LOG(Hardware,
               "❌ CRITICAL: DMA buffer misaligned! paddr=0x%llx requested=%zu actual=%llu",
               mappedAddress, alignment, mappedAddress & (alignment - 1));
        command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        buffer->release();
        return std::nullopt;
    }

    // CRITICAL: Do NOT call CompleteDMA() - we must keep the IODMACommand alive
    // to maintain the IOMMU mapping. CompleteDMA() will unmap the address and
    // the IOMMU will reuse it for the next allocation, causing address collisions.

    ASFW_LOG(Hardware,
           "DMA buffer allocated: IOMMU-mapped paddr=0x%llx size=%zu",
           mappedAddress, length);

    return DMABuffer{
        .descriptor = OSSharedPtr(buffer, OSNoRetain),
        .dmaCommand = std::move(command),  // Transfer ownership - keeps mapping alive
        .deviceAddress = mappedAddress,
        .length = length
    };
}

OSSharedPtr<IODMACommand> HardwareInterface::CreateDMACommand() {
    if (!device_) {
        return nullptr;
    }

    // TODO: OHCI Spec - Validate DMA address bit limitations
    // - OHCI controllers typically support 32-bit DMA addresses
    // - kDefaultDMAMaxAddressBits = 32 is appropriate for most OHCI
    // - Some controllers may support more, should be configurable
    // - Linux uses dma_set_mask for this validation
    IODMACommandSpecification spec{};
    spec.maxAddressBits = kDefaultDMAMaxAddressBits;
    IODMACommand* command = nullptr;
    kern_return_t kr = IODMACommand::Create(device_.get(), kIODMACommandCreateNoOptions, &spec, &command);
    if (kr != kIOReturnSuccess || command == nullptr) {
        return nullptr;
    }
    return OSSharedPtr(command, OSNoRetain);
}

uint32_t HardwareInterface::ReadHCControl() const noexcept {
    return Read(Register32::kHCControl);
}

void HardwareInterface::SetHCControlBits(uint32_t bits) noexcept {
    WriteAndFlush(Register32::kHCControlSet, bits);
}

void HardwareInterface::ClearHCControlBits(uint32_t bits) noexcept {
    WriteAndFlush(Register32::kHCControlClear, bits);
}

uint32_t HardwareInterface::ReadNodeID() const noexcept {
    return Read(Register32::kNodeID);
}

namespace {

// Generic wait-for-register helper with device ejection detection and flexible logging.
// Template parameters:
//   ReadFn: callable returning uint32_t (reads the state register, NOT a strobe)
//   LogFn: callable(const char* name, uint32_t value, uint64_t attempts, uint64_t usec, bool ejected)
template <typename ReadFn, typename LogFn>
static bool WaitForRegister(ReadFn&& read32,
                            uint32_t mask,
                            bool expectSet,
                            uint32_t timeoutUsec,
                            uint32_t pollIntervalUsec,
                            const char* name,
                            LogFn&& logFn) {
    if (pollIntervalUsec == 0) {
        pollIntervalUsec = 100;
    }

    uint64_t waited = 0;
    uint64_t attempts = 0;

    while (timeoutUsec == 0 || waited < timeoutUsec) {
        const uint32_t value = read32();
        attempts++;

        // Detect device ejection: MMIO reads return 0xFFFFFFFF when device/BAR unmapped
        if (value == 0xFFFFFFFFu) {
            logFn(name, value, attempts, waited, /*ejected=*/true);
            return false;
        }

        const bool bitSet = (value & mask) == mask;
        if ((expectSet && bitSet) || (!expectSet && !bitSet)) {
            logFn(name, value, attempts, waited, /*ejected=*/false);
            return true;
        }

        if (waited + pollIntervalUsec > timeoutUsec && timeoutUsec != 0) {
            break;
        }

#ifndef ASFW_HOST_TEST
        IODelay(pollIntervalUsec);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(pollIntervalUsec));
#endif
        waited += pollIntervalUsec;
    }

    // Timeout: read final value for logging
    const uint32_t finalValue = read32();
    logFn(name, finalValue, attempts, waited, /*ejected=*/false);
    return false;
}

} // anonymous namespace

bool HardwareInterface::WaitHC(uint32_t mask,
                               bool expectSet,
                               uint32_t timeoutUsec,
                               uint32_t pollIntervalUsec) const {
    if (!device_) {
        return false;
    }

    return WaitForRegister(
        [this] { return Read(Register32::kHCControl); },
        mask, expectSet, timeoutUsec, pollIntervalUsec, "HCControl",
        [](const char* name, uint32_t value, uint64_t attempts, uint64_t usec, bool ejected) {
            if (ejected) {
                ASFW_LOG(Hardware, "%{public}s: device gone (0x%08x) tries=%llu t=%lluus",
                         name, value, attempts, usec);
            } else {
                const char* unit = (usec >= 1000) ? "ms" : "usec";
                const uint64_t t = (usec >= 1000) ? usec / 1000 : usec;
                ASFW_LOG(Hardware, "%{public}s: 0x%08x tries=%llu t=%llu%{public}s",
                         name, value, attempts, t, unit);
            }
        });
}

bool HardwareInterface::WaitLink(uint32_t mask,
                                 bool expectSet,
                                 uint32_t timeoutUsec,
                                 uint32_t pollIntervalUsec) const {
    if (!device_) {
        return false;
    }

    return WaitForRegister(
        [this] { return Read(Register32::kLinkControl); },
        mask, expectSet, timeoutUsec, pollIntervalUsec, "LinkControl",
        [](const char* name, uint32_t value, uint64_t attempts, uint64_t usec, bool ejected) {
            ASFW_LOG(Hardware, "%{public}s: 0x%08x tries=%llu t=%lluus ejected=%d",
                     name, value, attempts, usec, ejected);
        });
}

bool HardwareInterface::WaitNodeIdValid(uint32_t timeoutMs) const {
    if (!device_) {
        return false;
    }

    return WaitForRegister(
        [this] { return Read(Register32::kNodeID); },
        /*mask=*/0x80000000u, /*expectSet=*/true,
        /*timeoutUsec=*/timeoutMs * 1000, /*pollIntervalUsec=*/1000,
        "NodeID",
        [](const char* name, uint32_t value, uint64_t attempts, uint64_t usec, bool ejected) {
            const uint32_t bus  = (value >> 16) & 0x3FFu;
            const uint32_t node = (value >>  0) & 0x3Fu;
            const bool valid = (value & 0x80000000u) != 0;
            ASFW_LOG(Hardware, "%{public}s: 0x%08x valid=%d bus=%u node=%u tries=%llu t=%lluus ejected=%d",
                     name, value, valid, bus, node, attempts, usec, ejected);
        });
}

void HardwareInterface::FlushPostedWrites() const {
    if (!device_) {
        return;
    }
    uint32_t value = 0;
    device_->MemoryRead32(barIndex_, static_cast<uint64_t>(Register32::kHCControl), &value);
    (void)value;
    FullBarrier();
}

} // namespace ASFW::Driver
