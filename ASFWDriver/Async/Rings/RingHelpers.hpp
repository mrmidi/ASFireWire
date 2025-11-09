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

namespace ASFW::Async::RingHelpers {

/**
 * \brief Calculate ring capacity accounting for the sentinel slot.
 *
 * Ring buffers typically reserve one slot to distinguish full from empty
 * (both would have head==tail otherwise). This returns usable capacity.
 *
 * \param storageSize Total number of slots in storage
 * \return Usable capacity (storageSize - 1) or 0 if storageSize is 0
 */
[[nodiscard]] constexpr inline size_t UsableCapacity(size_t storageSize) noexcept {
    return storageSize > 0 ? storageSize - 1 : 0;
}

/**
 * \brief Calculate number of elements in ring (distance from head to tail).
 *
 * Works for both atomic and non-atomic indices.
 *
 * \param head Current head index
 * \param tail Current tail index
 * \param capacity Ring capacity (storage size)
 * \return Number of elements currently in ring
 */
[[nodiscard]] constexpr inline size_t Count(size_t head, size_t tail, size_t capacity) noexcept {
    if (capacity == 0) return 0;
    return (capacity + tail - head) % capacity;
}

/**
 * \brief Check if ring is empty.
 *
 * \param head Current head index
 * \param tail Current tail index
 * \return true if ring contains no elements
 */
[[nodiscard]] constexpr inline bool IsEmpty(size_t head, size_t tail) noexcept {
    return head == tail;
}

/**
 * \brief Check if ring is full.
 *
 * Ring is full when advancing tail by 1 would equal head (sentinel slot).
 *
 * \param head Current head index
 * \param tail Current tail index
 * \param capacity Ring capacity (storage size)
 * \return true if ring is full
 */
[[nodiscard]] constexpr inline bool IsFull(size_t head, size_t tail, size_t capacity) noexcept {
    if (capacity == 0) return true;
    return ((tail + 1) % capacity) == head;
}

/**
 * \brief Advance index with wraparound.
 *
 * \param index Current index
 * \param amount Number of slots to advance
 * \param capacity Ring capacity (storage size)
 * \return New index after advancement with wraparound
 */
[[nodiscard]] constexpr inline size_t Advance(size_t index, size_t amount, size_t capacity) noexcept {
    if (capacity == 0) return 0;
    return (index + amount) % capacity;
}

/**
 * \brief Calculate space available for new elements.
 *
 * \param head Current head index
 * \param tail Current tail index
 * \param capacity Ring capacity (storage size)
 * \return Number of slots available (accounting for sentinel)
 */
[[nodiscard]] constexpr inline size_t Available(size_t head, size_t tail, size_t capacity) noexcept {
    if (capacity == 0) return 0;
    const size_t used = Count(head, tail, capacity);
    // Reserve 1 slot as sentinel (full vs empty distinction)
    return (capacity > used + 1) ? (capacity - used - 1) : 0;
}

//==============================================================================
// Atomic Variants (for lock-free rings like DescriptorRing)
//==============================================================================

/**
 * \brief Check if atomic ring is empty.
 *
 * \param head Atomic head index
 * \param tail Atomic tail index
 * \return true if ring contains no elements
 *
 * \par Memory Ordering
 * Uses acquire semantics to ensure visibility of writes.
 */
[[nodiscard]] inline bool IsEmptyAtomic(
    const std::atomic<size_t>& head,
    const std::atomic<size_t>& tail) noexcept
{
    return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
}

/**
 * \brief Check if atomic ring is full.
 *
 * \param head Atomic head index
 * \param tail Atomic tail index
 * \param capacity Ring capacity
 * \return true if ring is full
 *
 * \par Memory Ordering
 * Uses acquire semantics to ensure visibility of writes.
 */
[[nodiscard]] inline bool IsFullAtomic(
    const std::atomic<size_t>& head,
    const std::atomic<size_t>& tail,
    size_t capacity) noexcept
{
    const size_t h = head.load(std::memory_order_acquire);
    const size_t t = tail.load(std::memory_order_acquire);
    return IsFull(h, t, capacity);
}

/**
 * \brief Calculate count for atomic ring.
 *
 * \param head Atomic head index
 * \param tail Atomic tail index
 * \param capacity Ring capacity
 * \return Number of elements in ring
 *
 * \par Memory Ordering
 * Uses acquire semantics to ensure visibility of writes.
 */
[[nodiscard]] inline size_t CountAtomic(
    const std::atomic<size_t>& head,
    const std::atomic<size_t>& tail,
    size_t capacity) noexcept
{
    const size_t h = head.load(std::memory_order_acquire);
    const size_t t = tail.load(std::memory_order_acquire);
    return Count(h, t, capacity);
}

/**
 * \brief Calculate available space for atomic ring.
 *
 * \param head Atomic head index
 * \param tail Atomic tail index
 * \param capacity Ring capacity
 * \return Number of slots available
 *
 * \par Memory Ordering
 * Uses acquire semantics to ensure visibility of writes.
 */
[[nodiscard]] inline size_t AvailableAtomic(
    const std::atomic<size_t>& head,
    const std::atomic<size_t>& tail,
    size_t capacity) noexcept
{
    const size_t h = head.load(std::memory_order_acquire);
    const size_t t = tail.load(std::memory_order_acquire);
    return Available(h, t, capacity);
}

} // namespace ASFW::Async::RingHelpers
