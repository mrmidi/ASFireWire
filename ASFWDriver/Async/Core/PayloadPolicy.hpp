// PayloadPolicy.hpp - Modern C++23 payload ownership abstractions (Phase 1.3)
//
// Provides type-safe, zero-overhead payload management with RAII semantics.
// Enables compile-time ownership validation and automatic resource cleanup.
//
// Key features:
// - PayloadType concept for compile-time validation
// - Ownership enum for explicit ownership tracking
// - UniquePayload for automatic cleanup
// - Zero overhead (no runtime cost)
//
// Usage:
//   auto payload = UniquePayload<PayloadHandle>(dmaMgr, buffer, size);
//   transaction->SetPayload(std::move(payload));  // Transfer ownership
//   // Payload automatically released when transaction destroyed

#pragma once

#include <concepts>
#include <memory>
#include <span>
#include <cstdint>
#include <utility>

namespace ASFW::Async {

// Forward declarations
class DMAMemoryManager;
class PayloadHandle;

// ============================================================================
// Ownership Semantics
// ============================================================================

/// Compile-time ownership tracking for payload resources
/// Prevents lifetime bugs and clarifies ownership intent
enum class Ownership : uint8_t {
    /// Unique ownership - resource automatically released on destruction
    /// Use for: Transaction payloads, temporary buffers
    Unique,

    /// Borrowed reference - no cleanup on destruction
    /// Use for: Reading payload data, passing to callbacks
    Borrowed
};

// ============================================================================
// PayloadType Concept
// ============================================================================

/// Compile-time payload type validation
/// Ensures payload types provide standard buffer interface
///
/// Required operations:
/// - GetBuffer() -> std::span<uint8_t> (CPU-accessible view)
/// - GetIOVA() -> uint64_t (device-visible physical address)
/// - GetSize() -> size_t (buffer size in bytes)
/// - Release() -> void (cleanup resources)
/// - IsValid() -> bool (check if payload is allocated)
template<typename P>
concept PayloadType = requires(P& p, const P& cp) {
    // Buffer access
    { p.GetBuffer() } -> std::convertible_to<std::span<uint8_t>>;
    { cp.GetBuffer() } -> std::convertible_to<std::span<const uint8_t>>;

    // Address translation
    { cp.GetIOVA() } -> std::convertible_to<uint64_t>;

    // Size query
    { cp.GetSize() } -> std::convertible_to<size_t>;

    // Resource management
    { p.Release() } -> std::same_as<void>;
    { cp.IsValid() } -> std::convertible_to<bool>;
};

// ============================================================================
// UniquePayload - RAII Unique Ownership Wrapper
// ============================================================================

/// RAII wrapper for unique payload ownership
/// Automatically releases payload on destruction (similar to unique_ptr)
///
/// Usage:
///   UniquePayload<PayloadHandle> payload(handle);
///   transaction->SetPayload(std::move(payload));  // Transfer ownership
///   // Automatic cleanup when transaction destroyed
///
/// Benefits:
/// - Automatic resource cleanup (no manual Release() calls)
/// - Move-only semantics (cannot copy)
/// - Zero overhead (inline to raw operations)
/// - Compile-time type validation
template<PayloadType T>
class [[nodiscard]] UniquePayload {
public:
    /// Default constructor - empty payload
    constexpr UniquePayload() noexcept = default;

    /// Take ownership of payload
    explicit UniquePayload(T&& payload) noexcept
        : payload_(std::move(payload)), owns_(true) {}

    /// Destructor - automatically release if owned
    ~UniquePayload() noexcept {
        if (owns_ && payload_.IsValid()) {
            payload_.Release();
        }
    }

    // Non-copyable
    UniquePayload(const UniquePayload&) = delete;
    UniquePayload& operator=(const UniquePayload&) = delete;

    /// Move constructor - transfer ownership
    UniquePayload(UniquePayload&& other) noexcept
        : payload_(std::move(other.payload_)), owns_(other.owns_) {
        other.owns_ = false;  // Relinquish ownership
    }

    /// Move assignment - transfer ownership
    UniquePayload& operator=(UniquePayload&& other) noexcept {
        if (this != &other) {
            // Release our payload if we own it
            if (owns_ && payload_.IsValid()) {
                payload_.Release();
            }
            // Transfer ownership
            payload_ = std::move(other.payload_);
            owns_ = other.owns_;
            other.owns_ = false;
        }
        return *this;
    }

    /// Check if payload is valid (allocated)
    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return payload_.IsValid();
    }

    /// Check if we own the payload
    [[nodiscard]] constexpr bool Owns() const noexcept {
        return owns_;
    }

    /// Get mutable reference to payload (for writing data)
    [[nodiscard]] T& Get() noexcept {
        return payload_;
    }

    /// Get const reference to payload (for reading data)
    [[nodiscard]] const T& Get() const noexcept {
        return payload_;
    }

    /// Dereference operator (access payload like pointer)
    [[nodiscard]] T* operator->() noexcept {
        return &payload_;
    }

    [[nodiscard]] const T* operator->() const noexcept {
        return &payload_;
    }

    /// Boolean conversion (check validity)
    [[nodiscard]] explicit operator bool() const noexcept {
        return IsValid();
    }

    /// Release ownership without destroying (use sparingly!)
    /// Returns the payload, caller now responsible for cleanup
    [[nodiscard]] T Release() noexcept {
        owns_ = false;
        return std::move(payload_);
    }

    /// Reset to empty state (releases current payload if owned)
    void Reset() noexcept {
        if (owns_ && payload_.IsValid()) {
            payload_.Release();
        }
        payload_ = T{};
        owns_ = false;
    }

private:
    T payload_{};
    bool owns_{false};
};

// ============================================================================
// BorrowedPayload - Non-Owning Reference Wrapper
// ============================================================================

/// Non-owning reference to payload (borrowed)
/// Does NOT release payload on destruction
///
/// Usage:
///   BorrowedPayload<PayloadHandle> ref(transaction->GetPayload());
///   auto data = ref->GetBuffer();  // Read data
///   // No cleanup - original owner still responsible
///
/// Benefits:
/// - Clear intent: "I'm just reading, not taking ownership"
/// - Prevents accidental double-free
/// - Zero overhead (just a reference)
template<PayloadType T>
class BorrowedPayload {
public:
    /// Borrow reference to existing payload
    explicit BorrowedPayload(const T& payload) noexcept
        : payload_(payload) {}

    // Copyable (just a reference)
    BorrowedPayload(const BorrowedPayload&) = default;
    BorrowedPayload& operator=(const BorrowedPayload&) = default;

    // Movable
    BorrowedPayload(BorrowedPayload&&) noexcept = default;
    BorrowedPayload& operator=(BorrowedPayload&&) noexcept = default;

    // No destructor - borrowed reference, no cleanup

    /// Check if referenced payload is valid
    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return payload_.IsValid();
    }

    /// Get const reference to payload (read-only)
    [[nodiscard]] const T& Get() const noexcept {
        return payload_;
    }

    /// Dereference operator (read-only access)
    [[nodiscard]] const T* operator->() const noexcept {
        return &payload_;
    }

    /// Boolean conversion (check validity)
    [[nodiscard]] explicit operator bool() const noexcept {
        return IsValid();
    }

private:
    const T& payload_;
};

// ============================================================================
// Compile-Time Validation
// ============================================================================

// Note: PayloadType concept validation happens at template instantiation sites.
// We cannot validate PayloadHandle here due to forward declaration (circular dependency).
// The concept will enforce constraints when UniquePayload<PayloadHandle> is actually used.

// ============================================================================
// Usage Examples
// ============================================================================

#if 0  // Documentation only

// Example 1: Transaction owns payload
class Transaction {
    UniquePayload<PayloadHandle> payload_;

public:
    void SetPayload(UniquePayload<PayloadHandle>&& p) {
        payload_ = std::move(p);  // Transfer ownership
    }

    // Payload automatically released when Transaction destroyed
};

// Example 2: Reading payload data (borrowed)
void LogPayloadData(const PayloadHandle& payload) {
    BorrowedPayload borrowed(payload);
    auto data = borrowed->GetBuffer();
    // ... log data ...
    // No cleanup - we don't own it
}

// Example 3: Factory function returning unique payload
UniquePayload<PayloadHandle> AllocatePayload(DMAMemoryManager& dmaMgr, size_t size) {
    PayloadHandle handle = dmaMgr.AllocateBuffer(size);
    return UniquePayload<PayloadHandle>(std::move(handle));  // Transfer ownership to caller
}

// Example 4: Explicit ownership transfer
void SubmitTransaction(Transaction* txn, UniquePayload<PayloadHandle> payload) {
    // Caller transfers ownership to transaction
    txn->SetPayload(std::move(payload));
    // txn now owns payload, will clean up on destruction
}

#endif

} // namespace ASFW::Async
