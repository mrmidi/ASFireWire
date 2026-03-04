#include "AsyncSubsystem.hpp"

#include "Contexts/ATRequestContext.hpp"
#include "Contexts/ATResponseContext.hpp"

#include "../Logging/Logging.hpp"
#include "../Shared/Memory/DMAMemoryManager.hpp"

namespace ASFW::Async {

uint32_t AsyncSubsystem::DrainTxCompletions(const char* reason) {
    if (!tracking_) {
        return 0;
    }

    uint32_t drained = 0;
    // CRITICAL: Only call ScanCompletion() - it properly rejects evt_no_status
    // Never bypass ScanCompletion() checks or advance ring head directly.
    // ScanCompletion() will return nullopt for evt_no_status without advancing head.
    auto scanContext = [&](auto* ctx) {
        if (!ctx) {
            return;
        }
        while (auto completion = ctx->ScanCompletion()) {
            tracking_->OnTxCompletion(*completion);
            ++drained;
        }
    };

    scanContext(ResolveAtRequestContext());
    scanContext(ResolveAtResponseContext());

    if (drained > 0 && reason) {
        ASFW_LOG_V2(Async,
                 "DrainTxCompletions: reason=%{public}s drained=%u",
                 reason,
                 drained);
    } else if (reason && DMAMemoryManager::IsTracingEnabled()) {
        // Log when called but nothing drained (helps diagnose leaks)
        auto* atReq = ResolveAtRequestContext();
        if (atReq) {
            const auto& ring = atReq->Ring();
            ASFW_LOG(Async,
                     "DrainTxCompletions: reason=%{public}s drained=0 (ATReq head=%zu tail=%zu)",
                     reason, ring.Head(), ring.Tail());
        }
    }

    return drained;
}

ATRequestContext* AsyncSubsystem::ResolveAtRequestContext() noexcept {
    if (contextManager_) return contextManager_->GetAtRequestContext();
    return nullptr;
}

ATResponseContext* AsyncSubsystem::ResolveAtResponseContext() noexcept {
    if (contextManager_) return contextManager_->GetAtResponseContext();
    return nullptr;
}

ARRequestContext* AsyncSubsystem::ResolveArRequestContext() noexcept {
    if (contextManager_) return contextManager_->GetArRequestContext();
    return nullptr;
}

ARResponseContext* AsyncSubsystem::ResolveArResponseContext() noexcept {
    if (contextManager_) return contextManager_->GetArResponseContext();
    return nullptr;
}

void AsyncSubsystem::OnTxInterrupt() {
    if (!isRunning_ || is_bus_reset_in_progress_.load(std::memory_order_acquire)) {
        return;  // Ignore completions during bus reset
    }

    (void)DrainTxCompletions("irq");
}

void AsyncSubsystem::OnRxInterrupt(ARContextType /*contextType*/) {
    if (rxPath_) {
        rxPath_->ProcessARInterrupts(is_bus_reset_in_progress_, isRunning_, busResetCapture_.get());
    }

    // No bus-reset work here. AR IRQ ≠ bus reset.
}

} // namespace ASFW::Async
