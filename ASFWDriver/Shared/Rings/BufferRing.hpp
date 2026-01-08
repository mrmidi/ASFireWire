#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <memory>

#include <DriverKit/IOLib.h>

#include "../../Hardware/HWNamespaceAlias.hpp"

namespace ASFW::Shared {

class IDMAMemory;

struct FilledBufferInfo {
    void* virtualAddress;
    size_t startOffset;
    size_t bytesFilled;
    size_t descriptorIndex;
};

class BufferRing {
public:
    BufferRing() = default;
    ~BufferRing() = default;
    [[nodiscard]] bool Initialize(std::span<HW::OHCIDescriptor> descriptors, std::span<uint8_t> buffers, size_t bufferCount, size_t bufferSize) noexcept;
    [[nodiscard]] bool Finalize(uint64_t descriptorsPhysBase, uint64_t buffersPhysBase) noexcept;
    [[nodiscard]] std::optional<FilledBufferInfo> Dequeue() noexcept;
    [[nodiscard]] kern_return_t Recycle(size_t index) noexcept;
    [[nodiscard]] void* GetBufferAddress(size_t index) const noexcept;
    [[nodiscard]] size_t Head() const noexcept { return head_; }
    [[nodiscard]] size_t BufferCount() const noexcept { return bufferCount_; }
    [[nodiscard]] size_t BufferSize() const noexcept { return bufferSize_; }
    [[nodiscard]] uint32_t CommandPtrWord() const noexcept;
    void BindDma(IDMAMemory* dma) noexcept;
    void PublishAllDescriptorsOnce() noexcept;
    [[nodiscard]] void* DescriptorBaseVA() noexcept { return descriptors_.data(); }
    [[nodiscard]] const void* DescriptorBaseVA() const noexcept { return descriptors_.data(); }
    
    // Low-level access for custom programming (Isoch, etc.)
    [[nodiscard]] HW::OHCIDescriptor* GetDescriptor(size_t index) noexcept {
        if (index >= bufferCount_) return nullptr;
        return &descriptors_[index];
    }
    
    [[nodiscard]] uint64_t GetElementIOVA(size_t index) const noexcept {
        if (index >= bufferCount_) return 0;
        return bufIOVABase_ + (index * bufferSize_);
    }

    [[nodiscard]] uint64_t GetDescriptorIOVA(size_t index) const noexcept {
        if (index >= bufferCount_) return 0;
        return descIOVABase_ + (index * sizeof(HW::OHCIDescriptor));
    }

    [[nodiscard]] void* GetElementVA(size_t index) const noexcept {
        return GetBufferAddress(index);
    }
    
    [[nodiscard]] size_t Capacity() const noexcept { return bufferCount_; }
    BufferRing(const BufferRing&) = delete;
    BufferRing& operator=(const BufferRing&) = delete;
private:
    std::span<HW::OHCIDescriptor> descriptors_;
    std::span<uint8_t> buffers_;
    size_t bufferCount_{0};
    size_t bufferSize_{0};
    size_t head_{0};
    size_t last_dequeued_bytes_{0};
    uint32_t descIOVABase_{0};
    uint32_t bufIOVABase_{0};
    IDMAMemory* dma_{nullptr};
};

} // namespace ASFW::Shared
