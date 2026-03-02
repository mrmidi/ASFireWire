#include "../ConfigROMStager.hpp"
#include "../Common/ConfigROMConstants.hpp"
#include "MemoryMapView.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Driver {

ConfigROMStager::ConfigROMStager() = default;
ConfigROMStager::~ConfigROMStager() = default;

kern_return_t ConfigROMStager::Prepare(HardwareInterface& hw, size_t romBytes) {
    if (prepared_) {
        return kIOReturnSuccess;
    }

    IOBufferMemoryDescriptor* rawBuffer = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionInOut, romBytes, ASFW::ConfigROM::kRomAlignmentBytes, &rawBuffer);
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
    kr = dma_->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buffer_.get(), 0, romBytes,
                             &flags, &segCount, &segment);
    if (kr != kIOReturnSuccess || segCount < 1) {
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
        map_.reset();
        buffer_.reset();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoResources;
    }

    if ((segment.address & (ASFW::ConfigROM::kRomAlignmentBytes - 1)) != 0) {
        ASFW_LOG(Hardware, "Config ROM DMA address 0x%llx not 1KiB aligned", segment.address);
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
        map_.reset();
        buffer_.reset();
        return kIOReturnNotAligned;
    }

    if (segment.length < romBytes) {
        ASFW_LOG(Hardware, "Config ROM DMA segment too small (len=%llu expected>=%zu)",
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

    MemoryMapView view(*map_);
    auto romSpan = image.ImageNative();
    const size_t romBytes = romSpan.size() * sizeof(uint32_t);

    const auto capacity = view.Bytes().size_bytes();
    if (romBytes > capacity) {
        ASFW_LOG(Hardware, "Config ROM image (%zu bytes) exceeds staging buffer (%zu bytes)",
                 romBytes, capacity);
        return kIOReturnNoSpace;
    }

    ZeroBuffer();
    if (romBytes > 0) {
        std::memcpy(view.Bytes().data(), romSpan.data(), romBytes);

        auto bufferExp = view.Span<uint32_t>(romSpan.size());
        if (!bufferExp.has_value() || bufferExp->empty()) {
            return kIOReturnNotAligned;
        }

        savedHeader_ = (*bufferExp)[0];
        savedBusOptions_ = image.BusInfoQuad();
        (*bufferExp)[0] = 0;

        std::atomic_thread_fence(std::memory_order_seq_cst);

        auto syncExp = view.Span<const volatile uint32_t>(romSpan.size());
        if (!syncExp.has_value()) {
            return kIOReturnNotAligned;
        }

        for (size_t i = 0; i < syncExp->size(); ++i) {
            (void)(*syncExp)[i];
        }

        std::atomic_thread_fence(std::memory_order_seq_cst);

        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);

        uint32_t segCount = 1;
        IOAddressSegment segment{};
        uint64_t flags = 0;
        kr = dma_->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buffer_.get(), 0, romBytes,
                                 &flags, &segCount, &segment);

        if (kr != kIOReturnSuccess || segCount < 1 || segment.address != segment_.address) {
            ASFW_LOG_CONFIG_ROM("DMA re-prepare failed: kr=0x%08x segCount=%u addr=0x%llx", kr,
                                segCount, segment.address);
        }
    }

    if (segment_.address > 0xFFFFFFFFULL) {
        ASFW_LOG(Hardware, "Config ROM DMA address 0x%llx exceeds 32-bit range", segment_.address);
        return kIOReturnUnsupported;
    }

    if (!guidWritten_) {
        hw.WriteAndFlush(Register32::kGUIDHi, image.GuidHiQuad());
        hw.WriteAndFlush(Register32::kGUIDLo, image.GuidLoQuad());
        guidWritten_ = true;
    }

    hw.WriteAndFlush(Register32::kBusOptions, image.BusInfoQuad());

    hw.WriteAndFlush(Register32::kConfigROMHeader, image.HeaderQuad());

    const auto mapAddr = static_cast<uint32_t>(segment_.address);
    hw.WriteAndFlush(Register32::kConfigROMMap, mapAddr);

    return kIOReturnSuccess;
}

void ConfigROMStager::Teardown(HardwareInterface& hw) {
    if (prepared_) {
        ASFW_LOG(Hardware,
                 "ConfigROMStager: Tearing down - clearing ConfigROMMap and BIBimageValid");
        hw.ClearHCControlBits(HCControlBits::kBibImageValid);
        hw.WriteAndFlush(Register32::kConfigROMMap, 0);
    }

    if (dma_) {
        ASFW_LOG(Hardware, "ConfigROMStager: Completing DMA and releasing resources");
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
    }
    map_.reset();
    buffer_.reset();
    prepared_ = false;
    guidWritten_ = false;
    segment_ = {};
    dmaFlags_ = 0;
    ASFW_LOG(Hardware, "ConfigROMStager: Teardown complete");
}

kern_return_t ConfigROMStager::EnsurePrepared(HardwareInterface& hw) {
    return prepared_ ? kIOReturnSuccess : Prepare(hw);
}

void ConfigROMStager::ZeroBuffer() {
    if (!map_) {
        return;
    }
    MemoryMapView view(*map_);
    std::memset(view.Bytes().data(), 0, view.Bytes().size_bytes());
}

void ConfigROMStager::RestoreHeaderAfterBusReset() {
    if (!map_ || savedHeader_ == 0) {
        return;
    }

    MemoryMapView view(*map_);
    auto bufferExp = view.Span<uint32_t>(1);
    if (!bufferExp.has_value() || bufferExp->empty()) {
        return;
    }

    const uint32_t currentHeader = (*bufferExp)[0];
    (*bufferExp)[0] = savedHeader_;

    std::atomic_thread_fence(std::memory_order_seq_cst);
    auto syncExp = view.Span<const volatile uint32_t>(1);
    if (syncExp.has_value() && !syncExp->empty()) {
        (void)(*syncExp)[0];
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);

    ASFW_LOG(Hardware, "Config ROM header restored in DMA buffer: 0x%08x → 0x%08x", currentHeader,
             savedHeader_);
}

} // namespace ASFW::Driver
