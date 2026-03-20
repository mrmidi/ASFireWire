#pragma once

#include "IRMTypes.hpp"
#include "../Async/Interfaces/IFireWireBus.hpp"
#include <functional>
#include <memory>

namespace ASFW::IRM {

/**
 * Callback for IRM allocation operations.
 * Invoked asynchronously when allocation completes (success or failure).
 *
 * @param status Result of allocation operation
 */
using AllocationCallback = std::function<void(AllocationStatus status)>;
using CompareSwapCallback = std::function<void(AllocationStatus status, uint32_t oldValue)>;

struct ResourceSnapshot {
    uint32_t bandwidthAvailable{0};
    uint32_t channelsAvailable31_0{0};
    uint32_t channelsAvailable63_32{0};
};

using ResourceSnapshotCallback = std::function<void(AllocationStatus status, ResourceSnapshot snapshot)>;

class IRMClient {
public:
    explicit IRMClient(Async::IFireWireBus& bus);
    ~IRMClient();

    void SetIRMNode(uint8_t irmNodeId, Generation generation, uint64_t lastBusResetNs = 0);

    void AllocateChannel(uint8_t channel,
                        AllocationCallback callback,
                        const RetryPolicy& retryPolicy = RetryPolicy::Default());

    void ReleaseChannel(uint8_t channel,
                       AllocationCallback callback,
                       const RetryPolicy& retryPolicy = RetryPolicy::Default());

    void AllocateBandwidth(uint32_t units,
                          AllocationCallback callback,
                          const RetryPolicy& retryPolicy = RetryPolicy::Default());

    void ReleaseBandwidth(uint32_t units,
                         AllocationCallback callback,
                         const RetryPolicy& retryPolicy = RetryPolicy::Default());

    void AllocateResources(uint8_t channel,
                          uint32_t bandwidthUnits,
                          AllocationCallback callback,
                          const RetryPolicy& retryPolicy = RetryPolicy::Default());

    void ReleaseResources(uint8_t channel,
                         uint32_t bandwidthUnits,
                         AllocationCallback callback,
                         const RetryPolicy& retryPolicy = RetryPolicy::Default());

    void ReadResourcesSnapshot(ResourceSnapshotCallback callback);

    void CompareSwapBandwidth(uint32_t expected,
                              uint32_t desired,
                              CompareSwapCallback callback);

    void CompareSwapChannel(uint8_t channel,
                            uint32_t expected,
                            uint32_t desired,
                            CompareSwapCallback callback);

    [[nodiscard]] uint8_t GetIRMNodeID() const { return irmNodeId_; }

    [[nodiscard]] Generation GetGeneration() const { return generation_; }

private:
    struct ChannelLockState;
    struct BandwidthLockState;

    Async::IFireWireBus& bus_;

    uint8_t irmNodeId_{0xFF};
    Generation generation_{0};
    uint64_t lastBusResetNs_{0};

    void ReadIRMQuadlet(
        uint32_t addressLo,
        std::function<void(AllocationStatus status, uint32_t value)> callback);

    void CompareSwapIRMQuadlet(
        uint32_t addressLo,
        uint32_t expected,
        uint32_t desired,
        std::function<void(AllocationStatus status, uint32_t oldValue)> callback);

    void ReadIRMWindow(ResourceSnapshotCallback callback);
    void DelayForPostResetQuietPeriod() const;

    [[nodiscard]] static AllocationStatus MapAsyncStatus(Async::AsyncStatus status) noexcept;
    [[nodiscard]] static uint64_t CurrentMonotonicNowNs() noexcept;

    void PerformChannelLock(uint8_t channel, bool allocate,
                           AllocationCallback callback,
                           const RetryPolicy& retryPolicy);

    void StartChannelLock(const std::shared_ptr<ChannelLockState>& ctx);
    void OnChannelRead(const std::shared_ptr<ChannelLockState>& ctx,
                       bool success,
                       uint32_t currentValue);
    void OnChannelCompareSwap(const std::shared_ptr<ChannelLockState>& ctx,
                              uint32_t expectedValue,
                              bool success,
                              uint32_t oldValue);

    void PerformBandwidthLock(uint32_t units, bool allocate,
                             AllocationCallback callback,
                             const RetryPolicy& retryPolicy);

    void StartBandwidthLock(const std::shared_ptr<BandwidthLockState>& ctx);
    void OnBandwidthRead(const std::shared_ptr<BandwidthLockState>& ctx,
                         bool success,
                         uint32_t currentBandwidth);
    void OnBandwidthCompareSwap(const std::shared_ptr<BandwidthLockState>& ctx,
                                uint32_t expectedBandwidth,
                                bool success,
                                uint32_t oldValue);
};

} // namespace ASFW::IRM
