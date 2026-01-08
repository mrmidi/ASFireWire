#pragma once

#include "../Shared/Memory/IDMAMemory.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace ASFW::Testing {

/**
 * @brief Fake IDMAMemory implementation for host-side tests.
 *
 * Backed by a std::vector<uint8_t> slab with deterministic IOVA mapping.
 * Mirrors DMAMemoryManager allocation and translation semantics:
 * - Sequential cursor allocation, no frees
 * - Size rounded up to 16-byte alignment
 * - Alignment clamped to power-of-two, min 16
 * - Publish/Fetch modeled as full fences
 */
class FakeDMAMemory final : public Shared::IDMAMemory {
public:
    static constexpr size_t kDefaultSlabSize = 2 * 1024 * 1024;
    static constexpr uint64_t kBaseIOVA = 0x10000000ULL;

    explicit FakeDMAMemory(size_t totalSizeBytes = kDefaultSlabSize)
        : slab_(AlignSize(totalSizeBytes)),
          baseIOVA_(kBaseIOVA),
          cursor_(0) {}

    std::optional<Shared::DMARegion> AllocateRegion(size_t size, size_t alignment = 16) override {
        if (size == 0) return std::nullopt;

        if (alignment < 16) alignment = 16;
        if ((alignment & (alignment - 1)) != 0) alignment = 16;

        const size_t alignedSize = AlignSize(size);
        const size_t alignedCursor = (cursor_ + (alignment - 1)) & ~(alignment - 1);

        if (alignedCursor + alignedSize > slab_.size()) {
            return std::nullopt;
        }

        Shared::DMARegion region{};
        region.virtualBase = slab_.data() + alignedCursor;
        region.deviceBase = baseIOVA_ + alignedCursor;
        region.size = alignedSize;

        cursor_ = alignedCursor + alignedSize;
        return region;
    }

    uint64_t VirtToIOVA(const void* virt) const noexcept override {
        if (!IsInRange(virt)) return 0;
        auto* p = static_cast<const uint8_t*>(virt);
        return baseIOVA_ + static_cast<uint64_t>(p - slab_.data());
    }

    void* IOVAToVirt(uint64_t iova) const noexcept override {
        if (!IsInRange(iova)) return nullptr;
        return const_cast<uint8_t*>(slab_.data() + (iova - baseIOVA_));
    }

    void PublishToDevice(const void*, size_t) const noexcept override {
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    void FetchFromDevice(const void*, size_t) const noexcept override {
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    size_t TotalSize() const noexcept override { return slab_.size(); }
    size_t AvailableSize() const noexcept override { return slab_.size() - cursor_; }

    uint8_t* RawData() noexcept { return slab_.data(); }
    const uint8_t* RawData() const noexcept { return slab_.data(); }
    size_t Cursor() const noexcept { return cursor_; }

    void Reset() noexcept {
        cursor_ = 0;
        std::fill(slab_.begin(), slab_.end(), 0);
    }

    void InjectAt(size_t offset, const void* data, size_t length) {
        if (!data || length == 0) return;
        if (offset + length > slab_.size()) return;
        std::memcpy(slab_.data() + offset, data, length);
    }

private:
    static constexpr size_t AlignSize(size_t size) noexcept {
        return (size + 15) & ~size_t{15};
    }

    bool IsInRange(const void* ptr) const noexcept {
        if (slab_.empty() || !ptr) return false;
        auto* p = static_cast<const uint8_t*>(ptr);
        return p >= slab_.data() && p < slab_.data() + slab_.size();
    }

    bool IsInRange(uint64_t iova) const noexcept {
        return !slab_.empty() && iova >= baseIOVA_ && iova < baseIOVA_ + slab_.size();
    }

    mutable std::vector<uint8_t> slab_;
    uint64_t baseIOVA_;
    size_t cursor_;
};

} // namespace ASFW::Testing

