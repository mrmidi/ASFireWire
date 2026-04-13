// IsochTxRecoveryController.hpp
// ASFW - TX recovery state machine (watchdog-driven restart requests).

#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Isoch {

class IsochTxRecoveryController final {
public:
    // Reason bits (shared with verifier)
    static constexpr uint32_t kReasonInvalidLabel = 1u << 1;
    static constexpr uint32_t kReasonCipAnomaly = 1u << 2;
    static constexpr uint32_t kReasonDbcDiscontinuity = 1u << 3;
    static constexpr uint32_t kReasonUncompletedOverwrite = 1u << 4;
    static constexpr uint32_t kReasonInjectMiss = 1u << 5;

    static constexpr uint32_t kFatalMask =
        kReasonInvalidLabel |
        kReasonCipAnomaly |
        kReasonUncompletedOverwrite;

    void Request(uint32_t reasonBits) noexcept;

    /// Attempt to begin a restart. On success returns true and provides consumed reasons.
    /// The controller remains "in progress" until Complete() is called.
    [[nodiscard]] bool TryBegin(uint64_t nowNs, uint32_t& outReasons) noexcept;

    /// Complete the restart attempt and clear the in-progress gate.
    void Complete(uint64_t nowNs, uint32_t reasons, bool success) noexcept;

    [[nodiscard]] uint64_t RestartCount() const noexcept { return restartCount_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t SuppressedCount() const noexcept { return suppressedCount_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint32_t> requestBits_{0};
    std::atomic<uint64_t> lastRestartNs_{0};
    std::atomic<uint64_t> restartCount_{0};
    std::atomic<uint64_t> suppressedCount_{0};
    std::atomic_flag inProgress_ = ATOMIC_FLAG_INIT;
};

} // namespace ASFW::Isoch

