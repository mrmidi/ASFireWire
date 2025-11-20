#include "IRMClient.hpp"
#include "../Logging/Logging.hpp"
#include <DriverKit/IOLib.h>
#include <os/log.h>

namespace ASFW::IRM {

// ============================================================================
// Constructor / Destructor
// ============================================================================

IRMClient::IRMClient(Async::IFireWireBusOps& busOps)
    : busOps_(busOps)
{
}

IRMClient::~IRMClient() = default;

// ============================================================================
// Internal Helper Methods
// ============================================================================

void IRMClient::ReadIRMQuadlet(
    uint32_t addressLo,
    std::function<void(bool success, uint32_t value)> callback)
{
    // Build IRM CSR address
    Async::FWAddress addr{IRMRegisters::kAddressHi, addressLo};

    // IRM operations MUST use S100 per IEEE 1394 spec
    FW::FwSpeed speed{0};
    FW::NodeId node{irmNodeId_};
    FW::Generation gen{generation_};

    busOps_.ReadQuad(gen, node, addr, speed,
        [callback](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
                // Convert from big-endian bus order to host order
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                uint32_t hostValue = OSSwapBigToHostInt32(raw);
                callback(true, hostValue);
            } else {
                callback(false, 0);
            }
        });
}

void IRMClient::CompareSwapIRMQuadlet(
    uint32_t addressLo,
    uint32_t expected,
    uint32_t desired,
    std::function<void(bool success, uint32_t oldValue)> callback)
{
    // Build IRM CSR address
    Async::FWAddress addr{IRMRegisters::kAddressHi, addressLo};

    // IRM operations MUST use S100 per IEEE 1394 spec
    FW::FwSpeed speed{0};
    FW::NodeId node{irmNodeId_};
    FW::Generation gen{generation_};

    // Build CAS operand: [compare_value][swap_value] in big-endian
    std::array<uint8_t, 8> operand;
    uint32_t expectedBE = OSSwapHostToBigInt32(expected);
    uint32_t desiredBE = OSSwapHostToBigInt32(desired);
    std::memcpy(&operand[0], &expectedBE, 4);
    std::memcpy(&operand[4], &desiredBE, 4);

    busOps_.Lock(gen, node, addr, FW::LockOp::kCompareSwap,
        std::span{operand}, 4, speed,
        [callback, expected](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
                // Convert from big-endian bus order to host order
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                uint32_t oldValue = OSSwapBigToHostInt32(raw);

                // Success if old value matches expected (CAS succeeded)
                bool succeeded = (oldValue == expected);
                callback(succeeded, oldValue);
            } else {
                callback(false, 0);
            }
        });
}

// ============================================================================
// IRM Node Management
// ============================================================================

void IRMClient::SetIRMNode(uint8_t irmNodeId, Generation generation) {
    irmNodeId_ = irmNodeId;
    generation_ = generation;

    ASFW_LOG(IRM, "IRMClient: Set IRM node=%u generation=%u",
             irmNodeId, generation);
}

// ============================================================================
// Channel Allocation
// ============================================================================

void IRMClient::AllocateChannel(uint8_t channel,
                                 AllocationCallback callback,
                                 const RetryPolicy& retryPolicy)
{
    if (channel >= 64) {
        ASFW_LOG_ERROR(IRM, "AllocateChannel: Invalid channel %u", channel);
        callback(AllocationStatus::Failed);
        return;
    }

    if (irmNodeId_ == 0xFF) {
        ASFW_LOG_ERROR(IRM, "AllocateChannel: No IRM node on bus");
        callback(AllocationStatus::NotFound);
        return;
    }

    PerformChannelLock(channel, true /* allocate */, callback, retryPolicy);
}

void IRMClient::ReleaseChannel(uint8_t channel,
                                AllocationCallback callback,
                                const RetryPolicy& retryPolicy)
{
    if (channel >= 64) {
        ASFW_LOG_ERROR(IRM, "ReleaseChannel: Invalid channel %u", channel);
        callback(AllocationStatus::Failed);
        return;
    }

    if (irmNodeId_ == 0xFF) {
        ASFW_LOG_ERROR(IRM, "ReleaseChannel: No IRM node on bus");
        callback(AllocationStatus::NotFound);
        return;
    }

    PerformChannelLock(channel, false /* release */, callback, retryPolicy);
}

// ============================================================================
// Bandwidth Allocation
// ============================================================================

void IRMClient::AllocateBandwidth(uint32_t units,
                                   AllocationCallback callback,
                                   const RetryPolicy& retryPolicy)
{
    if (units == 0) {
        callback(AllocationStatus::Success);  // Nothing to allocate
        return;
    }

    if (irmNodeId_ == 0xFF) {
        ASFW_LOG_ERROR(IRM, "AllocateBandwidth: No IRM node on bus");
        callback(AllocationStatus::NotFound);
        return;
    }

    PerformBandwidthLock(units, true /* allocate */, callback, retryPolicy);
}

void IRMClient::ReleaseBandwidth(uint32_t units,
                                  AllocationCallback callback,
                                  const RetryPolicy& retryPolicy)
{
    if (units == 0) {
        callback(AllocationStatus::Success);  // Nothing to release
        return;
    }

    if (irmNodeId_ == 0xFF) {
        ASFW_LOG_ERROR(IRM, "ReleaseBandwidth: No IRM node on bus");
        callback(AllocationStatus::NotFound);
        return;
    }

    PerformBandwidthLock(units, false /* release */, callback, retryPolicy);
}

// ============================================================================
// Combined Resource Allocation (Two-Phase Commit with Modern C++23)
// ============================================================================

void IRMClient::AllocateResources(uint8_t channel,
                                   uint32_t bandwidthUnits,
                                   AllocationCallback callback,
                                   const RetryPolicy& retryPolicy)
{
    // Modern C++23: Use shared_ptr for RAII context management
    struct ResourceContext {
        uint8_t channel;
        uint32_t bandwidthUnits;
        RetryPolicy retryPolicy;
        AllocationCallback userCallback;
        bool channelAllocated{false};
    };

    auto ctx = std::make_shared<ResourceContext>(ResourceContext{
        channel, bandwidthUnits, retryPolicy, std::move(callback), false
    });

    // Phase 1: Allocate channel
    AllocateChannel(channel, [this, ctx](AllocationStatus status) {
        if (status != AllocationStatus::Success) {
            // Channel allocation failed, abort
            ctx->userCallback(status);
            return;
        }

        ctx->channelAllocated = true;

        // Phase 2: Allocate bandwidth
        AllocateBandwidth(ctx->bandwidthUnits,
            [this, ctx](AllocationStatus status) {
                if (status != AllocationStatus::Success) {
                    // Bandwidth allocation failed, rollback channel
                    ASFW_LOG(IRM, "AllocateResources: Bandwidth failed, rolling back channel %u",
                             ctx->channel);

                    ReleaseChannel(ctx->channel,
                        [ctx, status](AllocationStatus) {
                            // Report original bandwidth allocation failure
                            ctx->userCallback(status);
                        },
                        ctx->retryPolicy);
                } else {
                    // Success! Both allocated
                    ctx->userCallback(AllocationStatus::Success);
                }
            },
            ctx->retryPolicy);
    }, retryPolicy);
}

void IRMClient::ReleaseResources(uint8_t channel,
                                  uint32_t bandwidthUnits,
                                  AllocationCallback callback,
                                  const RetryPolicy& retryPolicy)
{
    // Modern C++23: Use shared_ptr for RAII context management
    struct ReleaseContext {
        AllocationCallback userCallback;
        uint8_t completionCount{0};
        AllocationStatus lastStatus{AllocationStatus::Success};
    };

    auto ctx = std::make_shared<ReleaseContext>(ReleaseContext{
        std::move(callback)
    });

    auto checkComplete = [ctx]() {
        if (ctx->completionCount == 2) {
            ctx->userCallback(ctx->lastStatus);
        }
    };

    // Release both in parallel
    ReleaseChannel(channel, [ctx, checkComplete](AllocationStatus status) {
        if (status != AllocationStatus::Success) {
            ctx->lastStatus = status;
        }
        ctx->completionCount++;
        checkComplete();
    }, retryPolicy);

    ReleaseBandwidth(bandwidthUnits, [ctx, checkComplete](AllocationStatus status) {
        if (status != AllocationStatus::Success) {
            ctx->lastStatus = status;
        }
        ctx->completionCount++;
        checkComplete();
    }, retryPolicy);
}

// ============================================================================
// Internal Implementation: Channel Lock (Modern C++23 with RAII)
// ============================================================================

void IRMClient::PerformChannelLock(uint8_t channel, bool allocate,
                                    AllocationCallback callback,
                                    const RetryPolicy& retryPolicy)
{
    // Determine which register to access
    const uint32_t addressLo = ChannelToRegisterAddress(channel);
    const uint32_t bitMask = ChannelToBitMask(channel);

    ASFW_LOG(IRM, "%{public}s channel %u (addr=0x%08x bit=0x%08x)",
             allocate ? "Allocating" : "Releasing",
             channel, addressLo, bitMask);

    // Modern C++23: Use shared_ptr for RAII context management
    struct LockContext {
        AllocationCallback userCallback;
        uint8_t channel;
        uint32_t addressLo;
        uint32_t bitMask;
        bool allocate;
        uint8_t retriesLeft;
    };

    auto ctx = std::make_shared<LockContext>(LockContext{
        std::move(callback),
        channel,
        addressLo,
        bitMask,
        allocate,
        retryPolicy.maxRetries
    });

    // Lambda to perform lock operation (with retry)
    // Capture this and ctx by value to ensure lifetime
    std::function<void()> performLock;
    performLock = [this, ctx, performLock]() {
        // Step 1: Read current value
        ReadIRMQuadlet(ctx->addressLo,
            [this, ctx, performLock](bool success, uint32_t value) {
                if (!success) {
                    ASFW_LOG_ERROR(IRM, "Channel read failed");
                    ctx->userCallback(AllocationStatus::Timeout);
                    return;
                }

                // Check availability and calculate new value
                uint32_t currentValue = value;
                uint32_t newValue;
                if (ctx->allocate) {
                    // Check if channel is available (bit set = available)
                    if ((currentValue & ctx->bitMask) == 0) {
                        ASFW_LOG(IRM, "Channel %u not available (current=0x%08x mask=0x%08x)",
                                 ctx->channel, currentValue, ctx->bitMask);
                        ctx->userCallback(AllocationStatus::NoResources);
                        return;
                    }
                    // Clear bit to allocate
                    newValue = currentValue & ~ctx->bitMask;
                } else {
                    // Set bit to release
                    newValue = currentValue | ctx->bitMask;
                }

                // Step 2: Perform compare-and-swap lock
                CompareSwapIRMQuadlet(ctx->addressLo, currentValue, newValue,
                    [this, ctx, currentValue, performLock](bool success, uint32_t oldValue) {
                        if (!success) {
                            ASFW_LOG_ERROR(IRM, "Channel lock operation failed");
                            ctx->userCallback(AllocationStatus::Timeout);
                            return;
                        }

                        // Check if CAS succeeded (old value matches expected)
                        if (oldValue == currentValue) {
                            // Lock succeeded!
                            ASFW_LOG(IRM, "Channel %u %{public}s succeeded",
                                     ctx->channel,
                                     ctx->allocate ? "allocation" : "release");
                            ctx->userCallback(AllocationStatus::Success);
                        } else {
                            // Contention: register changed between read and CAS
                            ASFW_LOG(IRM, "Channel lock contention "
                                     "(expected=0x%08x actual=0x%08x retries=%u)",
                                     currentValue, oldValue, ctx->retriesLeft);

                            if (ctx->retriesLeft > 0) {
                                ctx->retriesLeft--;
                                performLock();  // Retry immediately (Apple's approach)
                            } else {
                                ASFW_LOG(IRM, "Channel lock exhausted retries");
                                ctx->userCallback(AllocationStatus::NoResources);
                            }
                        }
                    });
            });
    };

    // Start the operation
    performLock();
}

// ============================================================================
// Internal Implementation: Bandwidth Lock (Modern C++23 with RAII)
// ============================================================================

void IRMClient::PerformBandwidthLock(uint32_t units, bool allocate,
                                      AllocationCallback callback,
                                      const RetryPolicy& retryPolicy)
{
    ASFW_LOG(IRM, "%{public}s bandwidth %u units",
             allocate ? "Allocating" : "Releasing", units);

    // Modern C++23: Use shared_ptr for RAII context management
    struct BandwidthContext {
        AllocationCallback userCallback;
        uint32_t units;
        bool allocate;
        uint8_t retriesLeft;
    };

    auto ctx = std::make_shared<BandwidthContext>(BandwidthContext{
        std::move(callback),
        units,
        allocate,
        retryPolicy.maxRetries
    });

    // Lambda to perform lock operation (with retry)
    // Capture this and ctx by value to ensure lifetime
    std::function<void()> performLock;
    performLock = [this, ctx, performLock]() {
        // Step 1: Read current bandwidth
        ReadIRMQuadlet(IRMRegisters::kBandwidthAvailable,
            [this, ctx, performLock](bool success, uint32_t value) {
                if (!success) {
                    ASFW_LOG_ERROR(IRM, "Bandwidth read failed");
                    ctx->userCallback(AllocationStatus::Timeout);
                    return;
                }

                uint32_t currentBandwidth = value;

                // Check availability and calculate new value
                uint32_t newBandwidth;
                if (ctx->allocate) {
                    // Check if enough bandwidth available
                    if (currentBandwidth < ctx->units) {
                        ASFW_LOG(IRM, "Insufficient bandwidth (available=%u needed=%u)",
                                 currentBandwidth, ctx->units);
                        ctx->userCallback(AllocationStatus::NoResources);
                        return;
                    }
                    // Subtract units to allocate
                    newBandwidth = currentBandwidth - ctx->units;
                } else {
                    // Add units to release
                    newBandwidth = currentBandwidth + ctx->units;
                }

                // Step 2: Perform compare-and-swap lock
                CompareSwapIRMQuadlet(IRMRegisters::kBandwidthAvailable,
                    currentBandwidth, newBandwidth,
                    [this, ctx, currentBandwidth, performLock](bool success, uint32_t oldValue) {
                        if (!success) {
                            ASFW_LOG_ERROR(IRM, "Bandwidth lock operation failed");
                            ctx->userCallback(AllocationStatus::Timeout);
                            return;
                        }

                        // Check if CAS succeeded (old value matches expected)
                        if (oldValue == currentBandwidth) {
                            // Lock succeeded!
                            ASFW_LOG(IRM, "Bandwidth %{public}s succeeded (%u units)",
                                     ctx->allocate ? "allocation" : "release",
                                     ctx->units);
                            ctx->userCallback(AllocationStatus::Success);
                        } else {
                            // Contention: register changed between read and CAS
                            ASFW_LOG(IRM, "Bandwidth lock contention "
                                     "(expected=%u actual=%u retries=%u)",
                                     currentBandwidth, oldValue, ctx->retriesLeft);

                            if (ctx->retriesLeft > 0) {
                                ctx->retriesLeft--;
                                performLock();  // Retry immediately (Apple's approach)
                            } else {
                                ASFW_LOG(IRM, "Bandwidth lock exhausted retries");
                                ctx->userCallback(AllocationStatus::NoResources);
                            }
                        }
                    });
            });
    };

    // Start the operation
    performLock();
}

} // namespace ASFW::IRM
