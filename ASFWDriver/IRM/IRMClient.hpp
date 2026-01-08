#pragma once

#include "IRMTypes.hpp"
#include "../Async/Interfaces/IFireWireBusOps.hpp"
#include <functional>
#include <memory>
#include <array>
// #include <libkern/OSByteOrder.h>

namespace ASFW::IRM {

/**
 * Callback for IRM allocation operations.
 * Invoked asynchronously when allocation completes (success or failure).
 *
 * @param status Result of allocation operation
 */
using AllocationCallback = std::function<void(AllocationStatus status)>;

class IRMClient {
public:
    explicit IRMClient(Async::IFireWireBusOps& busOps);
    ~IRMClient();

    void SetIRMNode(uint8_t irmNodeId, Generation generation);

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

    [[nodiscard]] uint8_t GetIRMNodeID() const { return irmNodeId_; }

    [[nodiscard]] Generation GetGeneration() const { return generation_; }

private:
    Async::IFireWireBusOps& busOps_;

    uint8_t irmNodeId_{0xFF};
    Generation generation_{0};

    void ReadIRMQuadlet(
        uint32_t addressLo,
        std::function<void(bool success, uint32_t value)> callback);

    void CompareSwapIRMQuadlet(
        uint32_t addressLo,
        uint32_t expected,
        uint32_t desired,
        std::function<void(bool success, uint32_t oldValue)> callback);

    void PerformChannelLock(uint8_t channel, bool allocate,
                           AllocationCallback callback,
                           const RetryPolicy& retryPolicy);

    void PerformBandwidthLock(uint32_t units, bool allocate,
                             AllocationCallback callback,
                             const RetryPolicy& retryPolicy);
};

} // namespace ASFW::IRM
