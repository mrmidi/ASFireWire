#include "ConfigROMStager.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <cstring>

#include "../Hardware/HardwareInterface.hpp"
#include "../Logging/Logging.hpp"
#include "../Hardware/RegisterMap.hpp"

namespace {

constexpr uint64_t kRomAlignment = 1024; // OHCI §5.5.6 requires 1 KiB alignment

} // namespace

namespace ASFW::Driver {

ConfigROMStager::ConfigROMStager() = default;
ConfigROMStager::~ConfigROMStager() = default;

kern_return_t ConfigROMStager::Prepare(HardwareInterface& hw, size_t romBytes) {
    if (prepared_) {
        return kIOReturnSuccess;
    }

    IOBufferMemoryDescriptor* rawBuffer = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, romBytes, kRomAlignment, &rawBuffer);
    if (kr != kIOReturnSuccess || rawBuffer == nullptr) {
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }
    buffer_ = OSSharedPtr(rawBuffer, OSNoRetain);
    buffer_->SetLength(romBytes);

    IOMemoryMap* rawMap = nullptr;
    kr = buffer_->CreateMapping(0, 0, 0, 0, 0, &rawMap);
    if (kr != kIOReturnSuccess || rawMap == nullptr) {
        buffer_.reset();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }
    map_ = OSSharedPtr(rawMap, OSNoRetain);

    ZeroBuffer();

    dma_ = hw.CreateDMACommand();
    if (!dma_) {
        map_.reset();
        buffer_.reset();
        return kIOReturnNoResources;
    }

    uint32_t segCount = 1;
    IOAddressSegment segment{};
    uint64_t flags = 0;
    kr = dma_->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                             buffer_.get(),
                             0,
                             romBytes,
                             &flags,
                             &segCount,
                             &segment);
    if (kr != kIOReturnSuccess || segCount < 1) {
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
        map_.reset();
        buffer_.reset();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoResources;
    }

    if ((segment.address & (kRomAlignment - 1)) != 0) {
        ASFW_LOG(Hardware, "Config ROM DMA address 0x%llx not 1KiB aligned", segment.address);
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
        map_.reset();
        buffer_.reset();
        return kIOReturnNotAligned;
    }

    if (segment.length < romBytes) {
        ASFW_LOG(Hardware,
                     "Config ROM DMA segment too small (len=%llu expected>=%zu)",
                     segment.length, romBytes);
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
        map_.reset();
        buffer_.reset();
        return kIOReturnNoResources;
    }

    segment_ = segment;
    dmaFlags_ = flags;
    prepared_ = true;
    // ZeroBuffer already called before PrepareForDMA to ensure physical page allocation
    return kIOReturnSuccess;
}

kern_return_t ConfigROMStager::StageImage(const ConfigROMBuilder& image, HardwareInterface& hw) {
    kern_return_t kr = EnsurePrepared(hw);
    if (kr != kIOReturnSuccess) {
        return kr;
    }

    if (!map_) {
        return kIOReturnNotReady;
    }

    auto romSpan = image.ImageNative();
    const size_t romBytes = romSpan.size() * sizeof(uint32_t);
    void* base = reinterpret_cast<void*>(map_->GetAddress());
    const size_t capacity = static_cast<size_t>(map_->GetLength());

    if (romBytes > capacity) {
        ASFW_LOG(Hardware,
                     "Config ROM image (%zu bytes) exceeds staging buffer (%zu bytes)",
                     romBytes, capacity);
        return kIOReturnNoSpace;
    }

    ZeroBuffer();
    if (romBytes > 0) {
        std::memcpy(base, romSpan.data(), romBytes);

        uint32_t* buffer = static_cast<uint32_t*>(base);
        savedHeader_ = buffer[0];
        savedBusOptions_ = image.BusInfoQuad();
        buffer[0] = 0;

        std::atomic_thread_fence(std::memory_order_seq_cst);

        volatile const uint32_t* sync = static_cast<volatile const uint32_t*>(base);
        for (size_t i = 0; i < romBytes / sizeof(uint32_t); i++) {
            (void)sync[i];
        }

        std::atomic_thread_fence(std::memory_order_seq_cst);

        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);

        uint32_t segCount = 1;
        IOAddressSegment segment{};
        uint64_t flags = 0;
        kr = dma_->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                 buffer_.get(),
                                 0,
                                 romBytes,
                                 &flags,
                                 &segCount,
                                 &segment);

        if (kr != kIOReturnSuccess || segCount < 1 || segment.address != segment_.address) {
            ASFW_LOG_CONFIG_ROM("DMA re-prepare failed: kr=0x%08x segCount=%u addr=0x%llx",
                                 kr, segCount, segment.address);
        }
    }

    if (segment_.address > 0xFFFFFFFFULL) {
        ASFW_LOG(Hardware,
               "Config ROM DMA address 0x%llx exceeds 32-bit range",
               segment_.address);
        return kIOReturnUnsupported;
    }

    static bool guidWritten = false;
    if (!guidWritten) {
        hw.WriteAndFlush(Register32::kGUIDHi, image.GuidHiQuad());
        hw.WriteAndFlush(Register32::kGUIDLo, image.GuidLoQuad());
        guidWritten = true;
    }

    hw.WriteAndFlush(Register32::kBusOptions, image.BusInfoQuad());
    
    hw.WriteAndFlush(Register32::kConfigROMHeader, image.HeaderQuad());

    const uint32_t mapAddr = static_cast<uint32_t>(segment_.address);
    hw.WriteAndFlush(Register32::kConfigROMMap, mapAddr);
    
    return kIOReturnSuccess;
}

void ConfigROMStager::Teardown(HardwareInterface& hw) {
    if (prepared_) {
        hw.ClearHCControlBits(HCControlBits::kBibImageValid);
        hw.WriteAndFlush(Register32::kConfigROMMap, 0);
    }

    if (dma_) {
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
    }
    map_.reset();
    buffer_.reset();
    prepared_ = false;
    segment_ = {};
    dmaFlags_ = 0;
}

kern_return_t ConfigROMStager::EnsurePrepared(HardwareInterface& hw) {
    return prepared_ ? kIOReturnSuccess : Prepare(hw);
}

void ConfigROMStager::ZeroBuffer() {
    if (!map_) {
        return;
    }
    std::memset(reinterpret_cast<void*>(map_->GetAddress()), 0, static_cast<size_t>(map_->GetLength()));
}

void ConfigROMStager::RestoreHeaderAfterBusReset() {
    if (!map_ || savedHeader_ == 0) {
        return;
    }

    void* base = reinterpret_cast<void*>(map_->GetAddress());
    uint32_t* buffer = static_cast<uint32_t*>(base);

    const uint32_t currentHeader = buffer[0];
    buffer[0] = savedHeader_;

    std::atomic_thread_fence(std::memory_order_seq_cst);
    volatile uint32_t sync = buffer[0];
    (void)sync;
    std::atomic_thread_fence(std::memory_order_seq_cst);

    ASFW_LOG(Hardware, "Config ROM header restored in DMA buffer: 0x%08x → 0x%08x",
             currentHeader, savedHeader_);
}

} // namespace ASFW::Driver
