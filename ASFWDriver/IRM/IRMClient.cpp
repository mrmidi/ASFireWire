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

void IRMClient::ReadIRMQuadlet(
    uint32_t addressLo,
    std::function<void(bool success, uint32_t value)> callback)
{
    Async::FWAddress addr{IRMRegisters::kAddressHi, addressLo};

    FW::FwSpeed speed{0};
    FW::NodeId node{irmNodeId_};
    FW::Generation gen{generation_};

    busOps_.ReadQuad(gen, node, addr, speed,
        [callback](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
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
    Async::FWAddress addr{IRMRegisters::kAddressHi, addressLo};

    FW::FwSpeed speed{0};
    FW::NodeId node{irmNodeId_};
    FW::Generation gen{generation_};

    std::array<uint8_t, 8> operand;
    uint32_t expectedBE = OSSwapHostToBigInt32(expected);
    uint32_t desiredBE = OSSwapHostToBigInt32(desired);
    std::memcpy(&operand[0], &expectedBE, 4);
    std::memcpy(&operand[4], &desiredBE, 4);

    busOps_.Lock(gen, node, addr, FW::LockOp::kCompareSwap,
        std::span{operand}, 4, speed,
        [callback, expected](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                uint32_t oldValue = OSSwapBigToHostInt32(raw);

                bool succeeded = (oldValue == expected);
                callback(succeeded, oldValue);
            } else {
                callback(false, 0);
            }
        });
}

void IRMClient::SetIRMNode(uint8_t irmNodeId, Generation generation) {
    irmNodeId_ = irmNodeId;
    generation_ = generation;

    ASFW_LOG(IRM, "IRMClient: Set IRM node=%u generation=%u",
             irmNodeId, generation);
}

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

    PerformChannelLock(channel, true, callback, retryPolicy);
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

    PerformChannelLock(channel, false, callback, retryPolicy);
}

void IRMClient::AllocateBandwidth(uint32_t units,
                                   AllocationCallback callback,
                                   const RetryPolicy& retryPolicy)
{
    if (units == 0) {
        callback(AllocationStatus::Success);
        return;
    }

    if (irmNodeId_ == 0xFF) {
        ASFW_LOG_ERROR(IRM, "AllocateBandwidth: No IRM node on bus");
        callback(AllocationStatus::NotFound);
        return;
    }

    PerformBandwidthLock(units, true, callback, retryPolicy);
}

void IRMClient::ReleaseBandwidth(uint32_t units,
                                  AllocationCallback callback,
                                  const RetryPolicy& retryPolicy)
{
    if (units == 0) {
        callback(AllocationStatus::Success);
        return;
    }

    if (irmNodeId_ == 0xFF) {
        ASFW_LOG_ERROR(IRM, "ReleaseBandwidth: No IRM node on bus");
        callback(AllocationStatus::NotFound);
        return;
    }

    PerformBandwidthLock(units, false, callback, retryPolicy);
}

void IRMClient::AllocateResources(uint8_t channel,
                                   uint32_t bandwidthUnits,
                                   AllocationCallback callback,
                                   const RetryPolicy& retryPolicy)
{
    struct ResourceContext {
        uint8_t channel;
        uint32_t bandwidthUnits;
        RetryPolicy retryPolicy;
        AllocationCallback userCallback;
        bool bandwidthAllocated{false};
    };

    auto ctx = std::make_shared<ResourceContext>(ResourceContext{
        channel, bandwidthUnits, retryPolicy, std::move(callback), false
    });

    AllocateBandwidth(bandwidthUnits, [this, ctx](AllocationStatus status) {
        if (status != AllocationStatus::Success) {
            ctx->userCallback(status);
            return;
        }

        ctx->bandwidthAllocated = true;

        AllocateChannel(ctx->channel,
            [this, ctx](AllocationStatus status) {
                if (status != AllocationStatus::Success) {
                    ASFW_LOG(IRM, "AllocateResources: Channel failed, rolling back %u bandwidth units",
                             ctx->bandwidthUnits);

                    ReleaseBandwidth(ctx->bandwidthUnits,
                        [ctx, status](AllocationStatus) {
                            ctx->userCallback(status);
                        },
                        ctx->retryPolicy);
                } else {
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

void IRMClient::PerformChannelLock(uint8_t channel, bool allocate,
                                    AllocationCallback callback,
                                    const RetryPolicy& retryPolicy)
{
    const uint32_t addressLo = ChannelToRegisterAddress(channel);
    const uint32_t bitMask = ChannelToBitMask(channel);

    ASFW_LOG(IRM, "%{public}s channel %u (addr=0x%08x bit=0x%08x)",
             allocate ? "Allocating" : "Releasing",
             channel, addressLo, bitMask);

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

    std::function<void()> performLock;
    performLock = [this, ctx, performLock]() {
        ReadIRMQuadlet(ctx->addressLo,
            [this, ctx, performLock](bool success, uint32_t value) {
                if (!success) {
                    ASFW_LOG_ERROR(IRM, "Channel read failed");
                    ctx->userCallback(AllocationStatus::Timeout);
                    return;
                }

                uint32_t currentValue = value;
                uint32_t newValue;
                if (ctx->allocate) {
                    if ((currentValue & ctx->bitMask) == 0) {
                        ASFW_LOG(IRM, "Channel %u not available (current=0x%08x mask=0x%08x)",
                                 ctx->channel, currentValue, ctx->bitMask);
                        ctx->userCallback(AllocationStatus::NoResources);
                        return;
                    }
                    newValue = currentValue & ~ctx->bitMask;
                } else {
                    newValue = currentValue | ctx->bitMask;
                }

                CompareSwapIRMQuadlet(ctx->addressLo, currentValue, newValue,
                    [this, ctx, currentValue, performLock](bool success, uint32_t oldValue) {
                        if (!success) {
                            ASFW_LOG_ERROR(IRM, "Channel lock operation failed");
                            ctx->userCallback(AllocationStatus::Timeout);
                            return;
                        }

                        if (oldValue == currentValue) {
                            ASFW_LOG(IRM, "Channel %u %{public}s succeeded",
                                     ctx->channel,
                                     ctx->allocate ? "allocation" : "release");
                            ctx->userCallback(AllocationStatus::Success);
                        } else {
                            ASFW_LOG(IRM, "Channel lock contention "
                                     "(expected=0x%08x actual=0x%08x retries=%u)",
                                     currentValue, oldValue, ctx->retriesLeft);

                            if (ctx->retriesLeft > 0) {
                                ctx->retriesLeft--;
                                performLock();
                            } else {
                                ASFW_LOG(IRM, "Channel lock exhausted retries");
                                ctx->userCallback(AllocationStatus::NoResources);
                            }
                        }
                    });
            });
    };

    performLock();
}

void IRMClient::PerformBandwidthLock(uint32_t units, bool allocate,
                                      AllocationCallback callback,
                                      const RetryPolicy& retryPolicy)
{
    ASFW_LOG(IRM, "%{public}s bandwidth %u units",
             allocate ? "Allocating" : "Releasing", units);

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

    std::function<void()> performLock;
    performLock = [this, ctx, performLock]() {
        ReadIRMQuadlet(IRMRegisters::kBandwidthAvailable,
            [this, ctx, performLock](bool success, uint32_t value) {
                if (!success) {
                    ASFW_LOG_ERROR(IRM, "Bandwidth read failed");
                    ctx->userCallback(AllocationStatus::Timeout);
                    return;
                }

                uint32_t currentBandwidth = value;

                uint32_t newBandwidth;
                if (ctx->allocate) {
                    if (currentBandwidth < ctx->units) {
                        ASFW_LOG(IRM, "Insufficient bandwidth (available=%u needed=%u)",
                                 currentBandwidth, ctx->units);
                        ctx->userCallback(AllocationStatus::NoResources);
                        return;
                    }
                    newBandwidth = currentBandwidth - ctx->units;
                } else {
                    newBandwidth = currentBandwidth + ctx->units;
                }

                CompareSwapIRMQuadlet(IRMRegisters::kBandwidthAvailable,
                    currentBandwidth, newBandwidth,
                    [this, ctx, currentBandwidth, performLock](bool success, uint32_t oldValue) {
                        if (!success) {
                            ASFW_LOG_ERROR(IRM, "Bandwidth lock operation failed");
                            ctx->userCallback(AllocationStatus::Timeout);
                            return;
                        }

                        if (oldValue == currentBandwidth) {
                            ASFW_LOG(IRM, "Bandwidth %{public}s succeeded (%u units)",
                                     ctx->allocate ? "allocation" : "release",
                                     ctx->units);
                            ctx->userCallback(AllocationStatus::Success);
                        } else {
                            ASFW_LOG(IRM, "Bandwidth lock contention "
                                     "(expected=%u actual=%u retries=%u)",
                                     currentBandwidth, oldValue, ctx->retriesLeft);

                            if (ctx->retriesLeft > 0) {
                                ctx->retriesLeft--;
                                performLock();
                            } else {
                                ASFW_LOG(IRM, "Bandwidth lock exhausted retries");
                                ctx->userCallback(AllocationStatus::NoResources);
                            }
                        }
                    });
            });
    };

    performLock();
}

} // namespace ASFW::IRM
