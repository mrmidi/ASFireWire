// LockPolicy.hpp - Modern C++23 locking abstractions (Phase 1.2)
//
// Provides type-safe, zero-overhead locking primitives with RAII semantics.
// Enables compile-time lock policy selection for testing and optimization.
//
// Key features:
// - LockPolicy concept for compile-time validation
// - ScopedLock RAII helper (automatic unlock)
// - IOLockWrapper for IOKit integration
// - NoLockPolicy for unit testing
// - Zero overhead (inlines to raw IOLock calls)
//
// Usage:
//   IOLockWrapper lock(IOLockAlloc());
//   {
//       ScopedLock guard(lock);  // Lock acquired
//       // ... critical section ...
//   }  // Lock automatically released

#pragma once

#include <concepts>
#include <DriverKit/IOLib.h>

namespace ASFW::Async {

// ============================================================================
// LockPolicy Concept
// ============================================================================

/// Compile-time lock policy validation
/// Ensures lock types provide standard mutex interface
template<typename L>
concept LockPolicy = requires(L& lock) {
    { lock.lock() } -> std::same_as<void>;
    { lock.unlock() } -> std::same_as<void>;
    { lock.try_lock() } -> std::same_as<bool>;
};

// ============================================================================
// IOLockWrapper - IOKit lock adapter
// ============================================================================

/// Wraps IOKit IOLock in standard mutex interface
/// Zero overhead: inlines to direct IOLock calls
struct IOLockWrapper {
    IOLock* lock_{nullptr};

    constexpr IOLockWrapper() noexcept = default;
    explicit IOLockWrapper(IOLock* l) noexcept : lock_(l) {}

    /// Acquire lock (blocking)
    void lock() noexcept {
        if (lock_) {
            IOLockLock(lock_);
        }
    }

    /// Release lock
    void unlock() noexcept {
        if (lock_) {
            IOLockUnlock(lock_);
        }
    }

    /// Try to acquire lock (non-blocking)
    /// \return true if lock acquired, false if already held
    bool try_lock() noexcept {
        return lock_ ? IOLockTryLock(lock_) : true;
    }

    /// Check if lock is allocated
    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return lock_ != nullptr;
    }

    /// Get raw IOLock pointer (for legacy code)
    [[nodiscard]] constexpr IOLock* Raw() const noexcept {
        return lock_;
    }
};

// Compile-time validation
static_assert(LockPolicy<IOLockWrapper>, "IOLockWrapper must satisfy LockPolicy");

// ============================================================================
// NoLockPolicy - For unit testing
// ============================================================================

/// No-op lock policy for unit testing (fake lock)
/// All operations compile to nothing (zero overhead)
struct NoLockPolicy {
    constexpr void lock() noexcept {}
    constexpr void unlock() noexcept {}
    constexpr bool try_lock() noexcept { return true; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return true; }
};

static_assert(LockPolicy<NoLockPolicy>, "NoLockPolicy must satisfy LockPolicy");

// ============================================================================
// ScopedLock - RAII lock guard
// ============================================================================

/// RAII lock guard with move semantics
/// Automatically unlocks on scope exit (exception-safe)
///
/// Usage:
///   IOLockWrapper lock(IOLockAlloc());
///   {
///       ScopedLock guard(lock);  // Acquires lock
///       // Critical section
///   }  // Automatically releases lock
///
/// Move semantics:
///   ScopedLock guard1(lock);
///   ScopedLock guard2 = std::move(guard1);  // Transfer ownership
///   // guard1 no longer owns lock, guard2 will unlock
template<LockPolicy MutexT>
class [[nodiscard]] ScopedLock {
public:
    /// Acquire lock on construction
    constexpr explicit ScopedLock(MutexT& m) noexcept
        : mutex_(m), locked_(true) {
        mutex_.lock();
    }

    /// Release lock on destruction (if still owned)
    constexpr ~ScopedLock() noexcept {
        if (locked_) {
            mutex_.unlock();
        }
    }

    // Non-copyable
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

    /// Move constructor - transfer lock ownership
    constexpr ScopedLock(ScopedLock&& other) noexcept
        : mutex_(other.mutex_), locked_(other.locked_) {
        other.locked_ = false;  // Relinquish ownership
    }

    /// Move assignment - transfer lock ownership
    constexpr ScopedLock& operator=(ScopedLock&& other) noexcept {
        if (this != &other) {
            // Release our lock if we own one
            if (locked_) {
                mutex_.unlock();
            }
            // Transfer ownership
            locked_ = other.locked_;
            other.locked_ = false;
        }
        return *this;
    }

    /// Manually unlock before scope exit (use sparingly)
    constexpr void unlock() noexcept {
        if (locked_) {
            mutex_.unlock();
            locked_ = false;
        }
    }

    /// Check if lock is currently held
    [[nodiscard]] constexpr bool owns_lock() const noexcept {
        return locked_;
    }

private:
    MutexT& mutex_;
    bool locked_;
};

// Compile-time validation
static_assert(sizeof(ScopedLock<IOLockWrapper>) <= 16,
              "ScopedLock must be lightweight (pointer + bool)");

// ============================================================================
// Deduction Guides (C++17)
// ============================================================================

template<typename MutexT>
ScopedLock(MutexT&) -> ScopedLock<MutexT>;

// ============================================================================
// Convenience Aliases
// ============================================================================

/// Standard scoped lock for IOKit
using IOScopedLock = ScopedLock<IOLockWrapper>;

/// No-op scoped lock for testing
using NoOpScopedLock = ScopedLock<NoLockPolicy>;

// ============================================================================
// Usage Examples
// ============================================================================

#if 0  // Documentation only

// Example 1: Basic usage
void Example_BasicUsage() {
    IOLockWrapper lock(IOLockAlloc());

    {
        ScopedLock guard(lock);  // Lock acquired
        // ... critical section ...
    }  // Lock automatically released
}

// Example 2: Early unlock
void Example_EarlyUnlock() {
    IOLockWrapper lock(IOLockAlloc());
    ScopedLock guard(lock);

    // Critical section
    DoSomething();

    // Release lock early (before scope exit)
    guard.unlock();

    // Do non-critical work without holding lock
    DoExpensiveOperation();
}

// Example 3: Move semantics
ScopedLock<IOLockWrapper> AcquireLock(IOLockWrapper& lock) {
    return ScopedLock{lock};  // Transfer ownership to caller
}

void Example_MoveSemantics() {
    IOLockWrapper lock(IOLockAlloc());
    ScopedLock guard = AcquireLock(lock);  // Ownership transferred
    // Lock held here
}  // Lock released

// Example 4: Fine-grained locking (Phase 1.2 pattern)
void Example_FineGrainedLocking() {
    IOLockWrapper lock(IOLockAlloc());

    // Check state under lock
    State currentState;
    {
        ScopedLock guard(lock);
        currentState = state_;
    }  // Lock released before expensive operation

    // Do expensive hardware operation WITHOUT holding lock
    PollHardware(7000);  // 7ms polling, lock NOT held!

    // Update state under lock
    {
        ScopedLock guard(lock);
        UpdateState(currentState);
    }  // Lock released
}

// Example 5: Unit testing with NoLockPolicy
template<LockPolicy Lock>
class Component {
    Lock lock_;

public:
    void DoWork() {
        ScopedLock guard(lock_);
        // ... work ...
    }
};

// Production: Uses IOLock
using ProductionComponent = Component<IOLockWrapper>;

// Testing: No actual locking (fast unit tests)
using TestComponent = Component<NoLockPolicy>;

#endif

} // namespace ASFW::Async
