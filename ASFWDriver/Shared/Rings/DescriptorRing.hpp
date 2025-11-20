#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../../Hardware/HWNamespaceAlias.hpp"
#include "RingHelpers.hpp"  // Shared ring utilities

namespace ASFW::Shared {

class DescriptorRing {
public:
    DescriptorRing() = default;
    ~DescriptorRing() = default;

    [[nodiscard]] bool Initialize(std::span<HW::OHCIDescriptor> descriptors) noexcept;
    [[nodiscard]] bool IsEmpty() const noexcept {
        return RingHelpers::IsEmptyAtomic(head_, tail_);
    }
    [[nodiscard]] bool IsFull() const noexcept;
    [[nodiscard]] size_t Capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] size_t Size() const noexcept;
    [[nodiscard]] HW::OHCIDescriptor* At(size_t index) noexcept;
    [[nodiscard]] const HW::OHCIDescriptor* At(size_t index) const noexcept;
    [[nodiscard]] size_t Head() const noexcept {
        return head_.load(std::memory_order_acquire);
    }
    [[nodiscard]] size_t Tail() const noexcept {
        return tail_.load(std::memory_order_acquire);
    }
    void SetHead(size_t newHead) noexcept { head_.store(newHead, std::memory_order_release); }
    void SetTail(size_t newTail) noexcept { tail_.store(newTail, std::memory_order_release); }
    void SetPrevLastBlocks(uint8_t blocks) noexcept { prev_last_blocks_.store(blocks, std::memory_order_release); }
    [[nodiscard]] uint8_t PrevLastBlocks() const noexcept { return prev_last_blocks_.load(std::memory_order_acquire); }
    bool LocatePreviousLast(size_t tailIndex, HW::OHCIDescriptor*& outDescriptor, size_t& outIndex, uint8_t& outBlocks) noexcept;
    bool LocatePreviousLast(size_t tailIndex, const HW::OHCIDescriptor*& outDescriptor, size_t& outIndex, uint8_t& outBlocks) const noexcept;
    [[nodiscard]] std::span<HW::OHCIDescriptor> Storage() noexcept { return storage_; }
    [[nodiscard]] std::span<const HW::OHCIDescriptor> Storage() const noexcept { return storage_; }
    [[nodiscard]] bool Finalize(uint64_t descriptorsIOVABase) noexcept;
    [[nodiscard]] uint32_t CommandPtrWordTo(const HW::OHCIDescriptor* target, uint8_t zBlocks) const noexcept;
    [[nodiscard]] uint32_t CommandPtrWordFromIOVA(uint32_t iova32, uint8_t zBlocks) const noexcept;
    DescriptorRing(const DescriptorRing&) = delete;
    DescriptorRing& operator=(const DescriptorRing&) = delete;
private:
    std::span<HW::OHCIDescriptor> storage_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    std::atomic<uint8_t> prev_last_blocks_{0};
    size_t capacity_{0};
    uint64_t descIOVABase_{0};
};

} // namespace ASFW::Shared
