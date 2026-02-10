#include "HardwareInterface.hpp"

#include <algorithm>

#include "Logging.hpp"
#include "../Async/AsyncSubsystem.hpp"

#ifndef ASFW_HOST_TEST
#include <DriverKit/IOService.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSAction.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>
#else
#include <chrono>
#include <thread>
#endif

namespace {
struct IOLockGuard {
    IOLock* lock;
    explicit IOLockGuard(IOLock* l) : lock(l) { if (lock) IOLockLock(lock); }
    ~IOLockGuard() { if (lock) IOLockUnlock(lock); }
    IOLockGuard(const IOLockGuard&) = delete;
    IOLockGuard& operator=(const IOLockGuard&) = delete;
};
}

namespace ASFW::Driver {

namespace {
constexpr uint8_t kDefaultBAR = 0;
constexpr uint64_t kDefaultDMAMaxAddressBits = 32;
#ifndef ASFW_HOST_TEST
constexpr uint16_t kRequiredCommandBits = kIOPCICommandBusMaster | kIOPCICommandMemorySpace;
#else
constexpr uint16_t kRequiredCommandBits = 0;
#endif
}

HardwareInterface::HardwareInterface() {
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
    uint16_t vendorId = 0, deviceId = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vendorId);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &deviceId);
    
    quirk_agere_lsi_ = (vendorId == 0x11c1 && (deviceId == 0x5901 || deviceId == 0x5900));
    if (quirk_agere_lsi_) {
        ASFW_LOG(Hardware, "⚠️  Agere/LSI chipset detected");
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

void HardwareInterface::SetAsyncSubsystem(ASFW::Async::AsyncSubsystem* subsystem) noexcept {
    asyncSubsystem_ = subsystem;
}

uint32_t HardwareInterface::Read(Register32 reg) const noexcept {
    if (!device_) {
        return 0;
    }
    uint32_t value = 0;
    device_->MemoryRead32(barIndex_, static_cast<uint64_t>(reg), &value);
    return value;
}

void HardwareInterface::Write(Register32 reg, uint32_t value) noexcept {
    if (!device_) {
        return;
    }
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
    snapshot.intMask = 0;
    device_->MemoryRead32(barIndex_, static_cast<uint64_t>(Register32::kIsoXmitEvent), &snapshot.isoXmitEvent);
    device_->MemoryRead32(barIndex_, static_cast<uint64_t>(Register32::kIsoRecvEvent), &snapshot.isoRecvEvent);
    return snapshot;
}

bool HardwareInterface::SendPhyConfig(std::optional<uint8_t> gapCount,
                                      std::optional<uint8_t> forceRootPhyId,
                                      std::string_view caller) {
    if (!device_) {
        return false;
    }
    if (!asyncSubsystem_) {
        ASFW_LOG_ERROR(Hardware, "PHY CONFIG send aborted - AsyncSubsystem not bound");
        return false;
    }

    AlphaPhyConfig config{};

    if (forceRootPhyId.has_value()) {
        config.rootId = static_cast<uint8_t>(*forceRootPhyId & 0x3Fu);
        config.forceRoot = true;
    }

    if (gapCount.has_value()) {
        uint8_t gap = static_cast<uint8_t>(*gapCount & 0x3Fu);
        if (gap == 0) {
            ASFW_LOG_ERROR(Hardware, "Rejecting PHY CONFIG gap update with value 0");
            return false;
        }
        config.gapCountOptimization = true;
        config.gapCount = gap;
    }

    if (!config.forceRoot && !config.gapCountOptimization) {
        ASFW_LOG(Hardware, "PHY CONFIG skipped - no requested changes");
        return false;
    }

    const auto quadlets = AlphaPhyConfigPacket{config}.EncodeBusOrder();

    ASFW_LOG(Hardware,
             "PHY CONFIG (forceRoot=%d root=%u gapUpdate=%d gap=%u) quad=0x%08x",
             config.forceRoot,
             config.rootId,
             config.gapCountOptimization,
             config.gapCount,
             quadlets[0]);

    ASFW::Async::PhyParams params{};
    params.quadlet1 = quadlets[0];
    params.quadlet2 = quadlets[1];

    auto completion = [packetQuad = quadlets[0]](ASFW::Async::AsyncHandle handle,
                                                 ASFW::Async::AsyncStatus status,
                                                 std::span<const uint8_t> /*response*/) {
        if (status == ASFW::Async::AsyncStatus::kSuccess) {
            ASFW_LOG(Hardware,
                     "PHY CONFIG complete handle=0x%x quad=0x%08x",
                     handle.value,
                     packetQuad);
        } else {
            ASFW_LOG_ERROR(Hardware,
                           "PHY CONFIG handle=0x%x failed status=%u quad=0x%08x",
                           handle.value,
                           static_cast<unsigned>(status),
                           packetQuad);
        }
    };

    const auto handle = asyncSubsystem_->PhyRequest(params, std::move(completion));
    if (!handle) {
        ASFW_LOG_ERROR(Hardware,
                       "PHY CONFIG submission rejected (handle=0) quad=0x%08x",
                       quadlets[0]);
        return false;
    }

    ASFW_LOG(Hardware,
             "PHY CONFIG submitted handle=0x%x data=(0x%08x, 0x%08x)",
             handle.value,
             params.quadlet1,
             params.quadlet2);
    return true;
}

bool HardwareInterface::SendPhyGlobalResume(uint8_t phyId) {
    if (!device_) {
        return false;
    }
    if (!asyncSubsystem_) {
        ASFW_LOG_ERROR(Hardware, "PHY GLOBAL RESUME aborted - AsyncSubsystem not bound");
        return false;
    }

    PhyGlobalResumePacket packet{};
    packet.phyId = static_cast<uint8_t>(phyId & 0x3Fu);
    const auto quadlets = packet.EncodeBusOrder();

    ASFW_LOG(Hardware, "PHY GLOBAL RESUME packet: phyId=%u quad=0x%08x", packet.phyId, quadlets[0]);

    ASFW::Async::PhyParams params{};
    params.quadlet1 = quadlets[0];
    params.quadlet2 = quadlets[1];

    auto completion = [packetQuad = quadlets[0]](ASFW::Async::AsyncHandle handle,
                                                 ASFW::Async::AsyncStatus status,
                                                 std::span<const uint8_t>) {
        if (status == ASFW::Async::AsyncStatus::kSuccess) {
            ASFW_LOG(Hardware, "PHY GLOBAL RESUME complete handle=0x%x quad=0x%08x", handle.value, packetQuad);
        } else {
            ASFW_LOG_ERROR(Hardware, "PHY GLOBAL RESUME handle=0x%x failed status=%u quad=0x%08x",
                           handle.value, static_cast<unsigned>(status), packetQuad);
        }
    };

    const auto handle = asyncSubsystem_->PhyRequest(params, std::move(completion));
    if (!handle) {
        ASFW_LOG_ERROR(Hardware, "PHY GLOBAL RESUME submission rejected (handle=0) quad=0x%08x", quadlets[0]);
        return false;
    }

    ASFW_LOG(Hardware, "PHY GLOBAL RESUME submitted handle=0x%x", handle.value);
    return true;
}

bool HardwareInterface::InitiateBusReset(bool shortReset) {
    (void)shortReset;
    return UpdatePhyRegister(1, 0, 0x40);
}

void HardwareInterface::SetContender(bool enable) {
    uint8_t newValue = enable ? (phyReg4Cache_ | 0x40) : (phyReg4Cache_ & 0xBF);

    if (WritePhyRegister(4, newValue)) {
        phyReg4Cache_ = newValue;
        ASFW_LOG(Hardware, "PHY Register 4 updated: Contender=%d (0x%02x)", enable, newValue);
    } else {
        ASFW_LOG_ERROR(Hardware, "Failed to update PHY Register 4");
    }
}

void HardwareInterface::InitializePhyReg4Cache() {
    const auto value = ReadPhyRegister(4);
    if (value.has_value()) {
        phyReg4Cache_ = *value;
        ASFW_LOG_V2(Hardware, "PHY Register 4 cache initialized: 0x%02x", *value);
    } else {
        ASFW_LOG_ERROR(Hardware, "Failed to initialize PHY Register 4 cache");
    }
}

void HardwareInterface::SetRootHoldOff(bool enable) {
    const auto currentOpt = ReadPhyRegister(1);
    if (!currentOpt.has_value()) {
        ASFW_LOG_ERROR(Hardware, "Failed to read PHY Register 1 for SetRootHoldOff(%d)", enable);
        return;
    }

    const uint8_t current = currentOpt.value();
    const bool rhbSet = (current & 0x80) != 0;

    if (enable) {
        if (rhbSet) {
            ASFW_LOG(Hardware, "PHY Register 1 RHB already set (0x%02x)", current);
            return;
        }

        const uint8_t newValue = current | 0x80;
        if (WritePhyRegister(1, newValue)) {
            ASFW_LOG(Hardware, "PHY Register 1 RHB enabled");
        } else {
            ASFW_LOG_ERROR(Hardware, "Failed to enable RHB");
        }
    } else {
        if (!rhbSet) {
            ASFW_LOG(Hardware, "PHY Register 1 RHB already clear (0x%02x)", current);
            return;
        }

        ASFW_LOG(Hardware, "PHY Register 1 RHB set, triggering bus reset to clear");
        InitiateBusReset(false);
    }
}

std::optional<uint8_t> HardwareInterface::ReadPhyRegister(uint8_t address) {
    IOLockGuard guard(phyLock_);
    return ReadPhyRegisterUnlocked(address);
}

std::optional<uint8_t> HardwareInterface::ReadPhyRegisterUnlocked(uint8_t address) {
    const uint32_t phyControl = (static_cast<uint32_t>(address) << 8) | 0x8000u;

    Write(Register32::kPhyControl, phyControl);
    FlushPostedWrites();

    ASFW_LOG_PHY("[PHY] Read reg %u: wrote PhyControl=0x%08x", address, phyControl);

    constexpr int kImmediateTries = 3;
    constexpr int kTotalTries = 103;

    for (int i = 0; i < kTotalTries; i++) {
        const uint32_t val = Read(Register32::kPhyControl);

        if (val == 0xFFFFFFFF) {
            ASFW_LOG(Hardware, "[PHY] Read reg %u failed - card ejected", address);
            return std::nullopt;
        }

        if (val & 0x80000000u) {
            const uint8_t data = static_cast<uint8_t>((val >> 16) & 0xFF);
            ASFW_LOG_PHY("[PHY] Read reg %u success: 0x%02x", address, data);
            return data;
        }

        if (i >= kImmediateTries) {
            IOSleep(1);
        }
    }

    ASFW_LOG(Hardware, "[PHY] Read reg %u TIMEOUT", address);
    return std::nullopt;
}

bool HardwareInterface::WritePhyRegister(uint8_t address, uint8_t value) {
    IOLockGuard guard(phyLock_);
    return WritePhyRegisterUnlocked(address, value);
}

bool HardwareInterface::WritePhyRegisterUnlocked(uint8_t address, uint8_t value) {
    const uint32_t phyControl = (static_cast<uint32_t>(address) << 8) |
                                static_cast<uint32_t>(value) | 0x4000u;

    Write(Register32::kPhyControl, phyControl);
    FlushPostedWrites();

    constexpr int kImmediateTries = 3;
    constexpr int kTotalTries = 103;

    for (int i = 0; i < kTotalTries; i++) {
        const uint32_t val = Read(Register32::kPhyControl);

        if (val == 0xFFFFFFFF) {
            ASFW_LOG(Hardware, "PHY write failed - card ejected");
            return false;
        }

        if ((val & 0x4000u) == 0) {
            ASFW_LOG_PHY("PHY[%u] write OK: 0x%02x", address, value);
            return true;
        }

        if (i >= kImmediateTries) {
            IOSleep(1);
        }
    }

    ASFW_LOG(Hardware, "PHY[%u] write timeout: 0x%02x", address, value);
    return false;
}

bool HardwareInterface::UpdatePhyRegister(uint8_t address, uint8_t clearBits, uint8_t setBits) {
    IOLockGuard guard(phyLock_);

    ASFW_LOG_PHY("Updating PHY[%u]: clear=0x%02x set=0x%02x", address, clearBits, setBits);

    const auto currentOpt = ReadPhyRegisterUnlocked(address);
    if (!currentOpt.has_value()) {
        ASFW_LOG_V0(Hardware, "PHY register %u update failed - read failed", address);
        return false;
    }

    uint8_t current = currentOpt.value();

    if (address == 5) {
        constexpr uint8_t kPhyIntStatusBits = 0x3C;
        clearBits |= kPhyIntStatusBits;
    }

    const uint8_t newValue = (current & ~clearBits) | setBits;

    ASFW_LOG_PHY("PHY register %u: 0x%02x → 0x%02x", address, current, newValue);

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
    if (!device_) {
        ASFW_LOG_V0(Hardware, "DMA allocation failed - no PCI device");
        return std::nullopt;
    }

    if ((options & (kIOMemoryDirectionOut | kIOMemoryDirectionIn)) != (kIOMemoryDirectionOut | kIOMemoryDirectionIn)) {
        ASFW_LOG(Hardware, "⚠️  AllocateDMA: options=0x%llx may not be bidirectional", options);
    }

    if (alignment == 0) alignment = 64;
    if (alignment < 16) alignment = 16;
    if ((alignment & (alignment - 1)) != 0) {
        ASFW_LOG_V0(Hardware, "AllocateDMA: alignment=%zu is not power-of-two", alignment);
        return std::nullopt;
    }

    IOBufferMemoryDescriptor* buffer = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(options, length, alignment, &buffer);
    if (kr != kIOReturnSuccess || buffer == nullptr) {
        ASFW_LOG_V0(Hardware, "IOBufferMemoryDescriptor::Create failed: 0x%08x", kr);
        return std::nullopt;
    }

    kr = buffer->SetLength(length);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_V0(Hardware, "IOBufferMemoryDescriptor::SetLength failed: 0x%08x", kr);
        buffer->release();
        return std::nullopt;
    }

    IODMACommandSpecification spec{};
    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = kDefaultDMAMaxAddressBits;

    IODMACommand* dmaCmd = nullptr;
    kr = IODMACommand::Create(device_.get(), kIODMACommandCreateNoOptions, &spec, &dmaCmd);
    if (kr != kIOReturnSuccess || dmaCmd == nullptr) {
        ASFW_LOG_V0(Hardware, "IODMACommand::Create failed: 0x%08x", kr);
        buffer->release();
        return std::nullopt;
    }
    OSSharedPtr<IODMACommand> command(dmaCmd, OSNoRetain);

    IOAddressSegment segments[32];
    uint32_t segmentCount = 32;
    uint64_t flags = 0;

    kr = command->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                buffer,
                                0,
                                length,
                                &flags,
                                &segmentCount,
                                segments);

    if (kr != kIOReturnSuccess) {
        ASFW_LOG_V0(Hardware, "IODMACommand::PrepareForDMA failed: 0x%08x", kr);
        command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        buffer->release();
        return std::nullopt;
    }

    if (segmentCount != 1) {
        ASFW_LOG_V0(Hardware, "❌ AllocateDMA: invalid segment count components=%u", segmentCount);
        command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        buffer->release();
        return std::nullopt;
    }
    
    if (segments[0].length < length) {
        ASFW_LOG_V0(Hardware, "❌ AllocateDMA: partial mapping len=%llu need=%zu",
                    (unsigned long long)segments[0].length, length);
        command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        buffer->release();
        return std::nullopt;
    }

    const uint64_t mappedAddress = segments[0].address;

    if (mappedAddress > 0xFFFFFFFFULL) {
        ASFW_LOG_V0(Hardware, "DMA IOVA 0x%llx exceeds 32-bit range", mappedAddress);
        command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        buffer->release();
        return std::nullopt;
    }

    if ((mappedAddress & (alignment - 1)) != 0) {
        ASFW_LOG_V0(Hardware, "❌ CRITICAL: DMA buffer misaligned! iova=0x%llx requested=%zu", mappedAddress, alignment);
        command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        buffer->release();
        return std::nullopt;
    }

    ASFW_LOG_V2(Hardware, "DMA buffer allocated: iova=0x%llx size=%zu align=%zu",
                mappedAddress, length, alignment);

    return DMABuffer{
        .descriptor = OSSharedPtr(buffer, OSNoRetain),
        .dmaCommand = std::move(command),
        .deviceAddress = mappedAddress,
        .length = length
    };
}

OSSharedPtr<IODMACommand> HardwareInterface::CreateDMACommand() {
    if (!device_) {
        return nullptr;
    }

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

std::pair<uint32_t, uint64_t> HardwareInterface::ReadCycleTimeAndUpTime() const noexcept {
    // Read cycle timer and capture host uptime as atomically as possible.
    // Per Apple's getCycleTimeAndUpTime(): read register first, then get uptime.
    // The order matters for accurate correlation between FireWire bus time and host time.
    const uint32_t cycleTimer = Read(Register32::kCycleTimer);
    const uint64_t uptime = mach_absolute_time();
    return {cycleTimer, uptime};
}

} // namespace ASFW::Driver
