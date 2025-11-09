#include "PayloadContext.hpp"
#include "../../Core/HardwareInterface.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IODMACommand.h>
#include <atomic>
#include <cstring>

namespace ASFW::Async {

// pImpl implementation - holds actual DMABuffer
struct PayloadContext::DMABufferImpl {
    ASFW::Driver::HardwareInterface::DMABuffer buffer;
};

std::unique_ptr<PayloadContext> PayloadContext::Create(
    ASFW::Driver::HardwareInterface& hw,
    const void* data,
    std::size_t length,
    uint64_t direction) {
    
    auto ctx = std::unique_ptr<PayloadContext>(new PayloadContext());
    if (!ctx->Initialize(hw, data, length, direction)) {
        return nullptr;
    }
    return ctx;
}

PayloadContext::~PayloadContext() {
    Cleanup();
}

uint64_t PayloadContext::DeviceAddress() const noexcept {
    return deviceAddress_;
}

std::shared_ptr<void> PayloadContext::IntoShared(std::unique_ptr<PayloadContext>&& up) {
    // Transfer ownership from unique_ptr to shared_ptr with custom deleter
    // Deleter casts void* back to PayloadContext* for proper destruction
    return std::shared_ptr<void>(
        up.release(),
        [](void* p) {
            delete static_cast<PayloadContext*>(p);
        }
    );
}

bool PayloadContext::Initialize(
    ASFW::Driver::HardwareInterface& hw,
    const void* data,
    std::size_t length,
    uint64_t direction) {
    
    dmaBufferImpl_ = std::make_unique<DMABufferImpl>();
    
    // Allocate DMA buffer via HardwareInterface
    auto dmaOpt = hw.AllocateDMA(length, direction, 16);
    if (!dmaOpt.has_value()) {
        ASFW_LOG_ERROR(Async, "PayloadContext: AllocateDMA failed for %zu bytes", length);
        return false;
    }
    
    dmaBufferImpl_->buffer = std::move(dmaOpt.value());
    
    // Map descriptor to get CPU-accessible virtual address
    IOMemoryMap* map = nullptr;
    kern_return_t kr = dmaBufferImpl_->buffer.descriptor->CreateMapping(0, 0, 0, 0, 0, &map);
    if (kr != kIOReturnSuccess || map == nullptr) {
        ASFW_LOG_ERROR(Async, "PayloadContext: CreateMapping failed kr=0x%x", kr);
        Cleanup();
        return false;
    }
    
    mapping_ = map;
    virtualAddress_ = reinterpret_cast<uint8_t*>(map->GetAddress());
    if (virtualAddress_ == nullptr) {
        ASFW_LOG_ERROR(Async, "PayloadContext: GetAddress returned null");
        Cleanup();
        return false;
    }
    
    // Copy source data into DMA buffer
    if (data != nullptr && length > 0) {
        std::memcpy(virtualAddress_, data, length);
        std::atomic_thread_fence(std::memory_order_release);

#if defined(IODMACommand_Synchronize_ID)
        if (dmaBufferImpl_->buffer.dmaCommand) {
            const kern_return_t syncKr = dmaBufferImpl_->buffer.dmaCommand->Synchronize(
                /*options*/0,
                /*offset*/0,
                static_cast<uint64_t>(length));
            if (syncKr != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Async,
                               "PayloadContext: Synchronize failed kr=0x%x (len=%zu)",
                               syncKr,
                               length);
                OSSynchronizeIO();
            }
        } else {
            ASFW_LOG_ERROR(Async, "PayloadContext: Missing DMA command for cache sync");
            OSSynchronizeIO();
        }
#else
        OSSynchronizeIO();
#endif
    }
    
    logicalAddress_ = data;
    length_ = length;
    deviceAddress_ = dmaBufferImpl_->buffer.deviceAddress;
    
    return true;
}

void PayloadContext::Cleanup() {
    // RAII cleanup - release resources in reverse order of acquisition
    
    if (mapping_ != nullptr) {
        mapping_->release();
        mapping_ = nullptr;
    }
    
    if (dmaBufferImpl_) {
        if (dmaBufferImpl_->buffer.dmaCommand) {
            dmaBufferImpl_->buffer.dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            dmaBufferImpl_->buffer.dmaCommand.reset();
        }
        
        dmaBufferImpl_->buffer.descriptor.reset();
        dmaBufferImpl_->buffer.deviceAddress = 0;
        dmaBufferImpl_->buffer.length = 0;
        dmaBufferImpl_.reset();
    }

    virtualAddress_ = nullptr;
    logicalAddress_ = nullptr;
    length_ = 0;
    deviceAddress_ = 0;
}

} // namespace ASFW::Async
