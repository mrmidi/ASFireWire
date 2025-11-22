#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Async {

// Forward-declare GenerationTracker in Bus namespace. GenerationTracker is the
// exclusive authority allowed to read/update the allocator's generation value.
namespace Bus { class GenerationTracker; }

// Allocates and tracks the IEEE 1394 asynchronous transaction labels (0-63).
class LabelAllocator {
public:
    LabelAllocator();
    ~LabelAllocator() = default;

    void Reset();
    uint8_t Allocate();
    void Free(uint8_t label);
    void ClearBitmap();  // Clear all allocation bits but keep generation as-is

    /**
     * \brief Get next label using simple counter rotation (0-63).
     *
     * Rotates through labels sequentially (2,3,4,5,...) for transaction hygiene.
     * This avoids label reuse when pipelining transactions, reducing risk of
     * mismatches with late/stale responses.
     *
     * \return Next label value (0-63), wraps around after 63
     *
     * \par Usage
     * Use this for sequential reads (e.g., Config ROM scanning) where labels
     * are not reused simultaneously. For concurrent requests, use Allocate()
     * which manages a bitmap to avoid collisions.
     */
    uint8_t NextLabel() noexcept;

    void BumpGeneration();
    // Note: CurrentGeneration()/SetGeneration() are intentionally private.
    // GenerationTracker is the only friend allowed to call them.
    bool IsLabelInUse(uint8_t label) const;

    static constexpr uint8_t kInvalidLabel = 0xFF;

    // Principle of Exclusive Authority:
    // Only GenerationTracker (ASFW::Async::Bus::GenerationTracker) is permitted
    // to read or update the internal generation value. This prevents fragmented
    // generation updates from multiple components and keeps interrupt-safe
    // semantics for OnSyntheticBusReset.
    friend class ASFW::Async::Bus::GenerationTracker;

private:
    // Generation API — private; only GenerationTracker may call these.
    uint16_t CurrentGeneration() const;
    void SetGeneration(uint16_t newGen);
    static constexpr uint8_t kMaxLabels = 64;
    static constexpr uint16_t kGenerationMask = 0x03FF; // 10-bit generation window

    std::atomic<uint64_t> bitmap_;
    std::atomic<uint16_t> generation_;
    std::atomic<uint8_t> next_label_{0};  ///< Simple counter for sequential label rotation
};

} // namespace ASFW::Async
