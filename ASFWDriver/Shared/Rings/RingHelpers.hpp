// RingHelpers.hpp - Shared utilities for ring buffer implementations (Phase 2.4)
//
// Provides common helper functions for circular ring buffer operations.
// Used by both DescriptorRing (AT context) and BufferRing (AR context) to eliminate
// code duplication while preserving their specialized behaviors.
//
// Design: Header-only inline functions for zero runtime overhead.

#pragma once

#include <cstddef>
#include <atomic>

namespace ASFW::Shared::RingHelpers {

[[nodiscard]] constexpr inline size_t UsableCapacity(size_t storageSize) noexcept {
    return storageSize > 0 ? storageSize - 1 : 0;
}

[[nodiscard]] constexpr inline size_t Count(size_t head, size_t tail, size_t capacity) noexcept {
    if (capacity == 0) return 0;
    return (capacity + tail - head) % capacity;
}

[[nodiscard]] constexpr inline bool IsEmpty(size_t head, size_t tail) noexcept {
    return head == tail;
}

[[nodiscard]] constexpr inline bool IsFull(size_t head, size_t tail, size_t capacity) noexcept {
    if (capacity == 0) return true;
    return ((tail + 1) % capacity) == head;
}

[[nodiscard]] constexpr inline size_t Advance(size_t index, size_t amount, size_t capacity) noexcept {
    if (capacity == 0) return 0;
    return (index + amount) % capacity;
}

[[nodiscard]] constexpr inline size_t Available(size_t head, size_t tail, size_t capacity) noexcept {
    if (capacity == 0) return 0;
    const size_t used = Count(head, tail, capacity);
    return (capacity > used + 1) ? (capacity - used - 1) : 0;
}

// Atomic variants
[[nodiscard]] inline bool IsEmptyAtomic(const std::atomic<size_t>& head, const std::atomic<size_t>& tail) noexcept {
    return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
}

[[nodiscard]] inline bool IsFullAtomic(const std::atomic<size_t>& head, const std::atomic<size_t>& tail, size_t capacity) noexcept {
    const size_t h = head.load(std::memory_order_acquire);
    const size_t t = tail.load(std::memory_order_acquire);
    return IsFull(h, t, capacity);
}

[[nodiscard]] inline size_t CountAtomic(const std::atomic<size_t>& head, const std::atomic<size_t>& tail, size_t capacity) noexcept {
    const size_t h = head.load(std::memory_order_acquire);
    const size_t t = tail.load(std::memory_order_acquire);
    return Count(h, t, capacity);
}

[[nodiscard]] inline size_t AvailableAtomic(const std::atomic<size_t>& head, const std::atomic<size_t>& tail, size_t capacity) noexcept {
    const size_t h = head.load(std::memory_order_acquire);
    const size_t t = tail.load(std::memory_order_acquire);
    return Available(h, t, capacity);
}

} // namespace ASFW::Shared::RingHelpers
