#pragma once

#include "../AsyncTypes.hpp"

#include <optional>

namespace ASFW::Debug {
class BusResetPacketCapture;
} // namespace ASFW::Debug

namespace ASFW::Async {

struct AsyncWatchdogStats {
    uint64_t tickCount{0};
    uint64_t expiredTransactions{0};
    uint64_t drainedTxCompletions{0};
    uint64_t contextsRearmed{0};
    uint64_t lastTickUsec{0};
};

/**
 * @brief Driver-internal async transaction port.
 *
 * Higher layers depend on this narrow boundary instead of the concrete async
 * engine implementation. The interface intentionally mirrors the current
 * transaction surface so refactors behind the port do not leak upward.
 *
 * Contract:
 * - submit methods invoke their completion exactly once;
 * - completions are delivered asynchronously relative to submit/cancel paths;
 * - returned handles are only valid for cancellation/lookup within the current
 *   async engine lifetime.
 */
class IAsyncSubsystemPort {
  public:
    virtual ~IAsyncSubsystemPort() = default;

    virtual AsyncHandle Read(const ReadParams& params, CompletionCallback callback) = 0;
    virtual AsyncHandle ReadWithRetry(const ReadParams& params, const RetryPolicy& retryPolicy,
                                      CompletionCallback callback) = 0;
    virtual AsyncHandle Write(const WriteParams& params, CompletionCallback callback) = 0;
    virtual AsyncHandle Lock(const LockParams& params, uint16_t extendedTCode,
                             CompletionCallback callback) = 0;
    virtual AsyncHandle CompareSwap(const CompareSwapParams& params,
                                    CompareSwapCallback callback) = 0;
    virtual AsyncHandle PhyRequest(const PhyParams& params, CompletionCallback callback) = 0;

    virtual bool Cancel(AsyncHandle handle) = 0;
    virtual void OnTimeoutTick() = 0;

    [[nodiscard]] virtual AsyncWatchdogStats GetWatchdogStats() const = 0;
    [[nodiscard]] virtual Debug::BusResetPacketCapture* GetBusResetCapture() const = 0;
    [[nodiscard]] virtual std::optional<AsyncStatusSnapshot> GetStatusSnapshot() const = 0;
};

} // namespace ASFW::Async
