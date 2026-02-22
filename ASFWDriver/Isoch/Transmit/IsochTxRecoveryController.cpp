// IsochTxRecoveryController.cpp

#include "IsochTxRecoveryController.hpp"

namespace ASFW::Isoch {

void IsochTxRecoveryController::Request(uint32_t reasonBits) noexcept {
    if (reasonBits == 0) {
        return;
    }
    requestBits_.fetch_or(reasonBits, std::memory_order_release);
}

bool IsochTxRecoveryController::TryBegin(uint64_t nowNs, uint32_t& outReasons) noexcept {
    outReasons = 0;

    const uint32_t reasonsPeek = requestBits_.load(std::memory_order_acquire);
    if (reasonsPeek == 0) {
        return false;
    }

    if (inProgress_.test_and_set(std::memory_order_acq_rel)) {
        return false;
    }

    // Cooldown to avoid restart storms.
    const uint64_t lastNs = lastRestartNs_.load(std::memory_order_relaxed);
    const uint64_t cooldownNs = (reasonsPeek & kFatalMask) ? 50'000'000ull : 200'000'000ull;
    if (lastNs != 0 && nowNs >= lastNs && (nowNs - lastNs) < cooldownNs) {
        suppressedCount_.fetch_add(1, std::memory_order_relaxed);
        inProgress_.clear(std::memory_order_release);
        return false;
    }

    const uint32_t reasons = requestBits_.exchange(0, std::memory_order_acq_rel);
    if (reasons == 0) {
        inProgress_.clear(std::memory_order_release);
        return false;
    }

    outReasons = reasons;
    return true;
}

void IsochTxRecoveryController::Complete(uint64_t nowNs, uint32_t reasons, bool success) noexcept {
    if (success) {
        lastRestartNs_.store(nowNs, std::memory_order_relaxed);
        restartCount_.fetch_add(1, std::memory_order_relaxed);
    } else if (reasons != 0) {
        // Retry next tick (subject to cooldown).
        requestBits_.fetch_or(reasons, std::memory_order_release);
    }

    inProgress_.clear(std::memory_order_release);
}

} // namespace ASFW::Isoch

