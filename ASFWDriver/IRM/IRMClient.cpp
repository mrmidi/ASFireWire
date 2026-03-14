#include "IRMClient.hpp"
#include "../Common/CallbackUtils.hpp"
#include "../Logging/Logging.hpp"
#include <DriverKit/IOLib.h>
#include <os/log.h>

namespace ASFW::IRM {

struct IRMClient::ChannelLockState {
    AllocationCallback userCallback;
    uint8_t channel{0};
    uint32_t addressLo{0};
    uint32_t bitMask{0};
    bool allocate{false};
    uint8_t retriesLeft{0};
};

struct IRMClient::BandwidthLockState {
    AllocationCallback userCallback;
    uint32_t units{0};
    bool allocate{false};
    uint8_t retriesLeft{0};
};

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
    auto callbackState = Common::ShareCallback(std::move(callback));
    Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = IRMRegisters::kAddressHi,
        .addressLo = addressLo,
    }};

    FW::FwSpeed speed{0};
    FW::NodeId node{irmNodeId_};
    FW::Generation gen{generation_};

    busOps_.ReadQuad(gen, node, addr, speed,
        [callbackState](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                uint32_t hostValue = OSSwapBigToHostInt32(raw);
                Common::InvokeSharedCallback(callbackState, true, hostValue);
            } else {
                Common::InvokeSharedCallback(callbackState, false, 0u);
            }
        });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void IRMClient::CompareSwapIRMQuadlet(
    uint32_t addressLo, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t expected,
    uint32_t desired,
    std::function<void(bool success, uint32_t oldValue)> callback)
{
    auto callbackState = Common::ShareCallback(std::move(callback));
    Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = IRMRegisters::kAddressHi,
        .addressLo = addressLo,
    }};

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
        [callbackState, expected](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                uint32_t oldValue = OSSwapBigToHostInt32(raw);

                bool succeeded = (oldValue == expected);
                Common::InvokeSharedCallback(callbackState, succeeded, oldValue);
            } else {
                Common::InvokeSharedCallback(callbackState, false, 0u);
            }
        });
}

void IRMClient::SetIRMNode(uint8_t irmNodeId, Generation generation) {
    irmNodeId_ = irmNodeId;
    generation_ = generation;

    ASFW_LOG(IRM, "IRMClient: Set IRM node=%u generation=%u",
             irmNodeId, generation.value);
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

    auto ctx = std::make_shared<ChannelLockState>(ChannelLockState{
        std::move(callback),
        channel,
        addressLo,
        bitMask,
        allocate,
        retryPolicy.maxRetries
    });

    StartChannelLock(ctx);
}

void IRMClient::PerformBandwidthLock(uint32_t units, bool allocate,
                                      AllocationCallback callback,
                                      const RetryPolicy& retryPolicy)
{
    ASFW_LOG(IRM, "%{public}s bandwidth %u units",
             allocate ? "Allocating" : "Releasing", units);

    auto ctx = std::make_shared<BandwidthLockState>(BandwidthLockState{
        std::move(callback),
        units,
        allocate,
        retryPolicy.maxRetries
    });

    StartBandwidthLock(ctx);
}

void IRMClient::StartChannelLock(const std::shared_ptr<ChannelLockState>& ctx) {
    ReadIRMQuadlet(ctx->addressLo, [this, ctx](bool success, uint32_t currentValue) {
        OnChannelRead(ctx, success, currentValue);
    });
}

void IRMClient::OnChannelRead(const std::shared_ptr<ChannelLockState>& ctx,
                              const bool success,
                              const uint32_t currentValue) {
    if (!success) {
        ASFW_LOG_ERROR(IRM, "Channel read failed");
        ctx->userCallback(AllocationStatus::Timeout);
        return;
    }

    uint32_t newValue = currentValue | ctx->bitMask;
    if (ctx->allocate) {
        if ((currentValue & ctx->bitMask) == 0) {
            ASFW_LOG(IRM, "Channel %u not available (current=0x%08x mask=0x%08x)",
                     ctx->channel, currentValue, ctx->bitMask);
            ctx->userCallback(AllocationStatus::NoResources);
            return;
        }
        newValue = currentValue & ~ctx->bitMask;
    }

    CompareSwapIRMQuadlet(ctx->addressLo, currentValue, newValue,
                          [this, ctx, currentValue](bool compareSwapSuccess, uint32_t oldValue) {
                              OnChannelCompareSwap(ctx, currentValue, compareSwapSuccess, oldValue);
                          });
}

void IRMClient::OnChannelCompareSwap(const std::shared_ptr<ChannelLockState>& ctx,
                                     const uint32_t expectedValue,
                                     const bool success,
                                     const uint32_t oldValue) {
    if (!success) {
        ASFW_LOG_ERROR(IRM, "Channel lock operation failed");
        ctx->userCallback(AllocationStatus::Timeout);
        return;
    }

    if (oldValue == expectedValue) {
        ASFW_LOG(IRM, "Channel %u %{public}s succeeded",
                 ctx->channel,
                 ctx->allocate ? "allocation" : "release");
        ctx->userCallback(AllocationStatus::Success);
        return;
    }

    ASFW_LOG(IRM, "Channel lock contention (expected=0x%08x actual=0x%08x retries=%u)",
             expectedValue, oldValue, ctx->retriesLeft);
    if (ctx->retriesLeft == 0) {
        ASFW_LOG(IRM, "Channel lock exhausted retries");
        ctx->userCallback(AllocationStatus::NoResources);
        return;
    }

    ctx->retriesLeft--;
    StartChannelLock(ctx);
}

void IRMClient::StartBandwidthLock(const std::shared_ptr<BandwidthLockState>& ctx) {
    ReadIRMQuadlet(IRMRegisters::kBandwidthAvailable,
                   [this, ctx](bool success, uint32_t currentBandwidth) {
                       OnBandwidthRead(ctx, success, currentBandwidth);
                   });
}

void IRMClient::OnBandwidthRead(const std::shared_ptr<BandwidthLockState>& ctx,
                                const bool success,
                                const uint32_t currentBandwidth) {
    if (!success) {
        ASFW_LOG_ERROR(IRM, "Bandwidth read failed");
        ctx->userCallback(AllocationStatus::Timeout);
        return;
    }

    uint32_t newBandwidth = currentBandwidth + ctx->units;
    if (ctx->allocate) {
        if (currentBandwidth < ctx->units) {
            ASFW_LOG(IRM, "Insufficient bandwidth (available=%u needed=%u)",
                     currentBandwidth, ctx->units);
            ctx->userCallback(AllocationStatus::NoResources);
            return;
        }
        newBandwidth = currentBandwidth - ctx->units;
    }

    CompareSwapIRMQuadlet(IRMRegisters::kBandwidthAvailable, currentBandwidth, newBandwidth,
                          [this, ctx, currentBandwidth](bool compareSwapSuccess, uint32_t oldValue) {
                              OnBandwidthCompareSwap(ctx, currentBandwidth, compareSwapSuccess, oldValue);
                          });
}

void IRMClient::OnBandwidthCompareSwap(const std::shared_ptr<BandwidthLockState>& ctx,
                                       const uint32_t expectedBandwidth,
                                       const bool success,
                                       const uint32_t oldValue) {
    if (!success) {
        ASFW_LOG_ERROR(IRM, "Bandwidth lock operation failed");
        ctx->userCallback(AllocationStatus::Timeout);
        return;
    }

    if (oldValue == expectedBandwidth) {
        ASFW_LOG(IRM, "Bandwidth %{public}s succeeded (%u units)",
                 ctx->allocate ? "allocation" : "release",
                 ctx->units);
        ctx->userCallback(AllocationStatus::Success);
        return;
    }

    ASFW_LOG(IRM, "Bandwidth lock contention (expected=%u actual=%u retries=%u)",
             expectedBandwidth, oldValue, ctx->retriesLeft);
    if (ctx->retriesLeft == 0) {
        ASFW_LOG(IRM, "Bandwidth lock exhausted retries");
        ctx->userCallback(AllocationStatus::NoResources);
        return;
    }

    ctx->retriesLeft--;
    StartBandwidthLock(ctx);
}

} // namespace ASFW::IRM
