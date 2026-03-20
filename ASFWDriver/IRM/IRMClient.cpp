#include "IRMClient.hpp"
#include "../Common/CallbackUtils.hpp"
#include "../Logging/Logging.hpp"
#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#endif
#include <DriverKit/IOLib.h>
#include <array>
#include <cstring>

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

IRMClient::IRMClient(Async::IFireWireBus& bus)
    : bus_(bus)
{
}

IRMClient::~IRMClient() = default;

AllocationStatus IRMClient::MapAsyncStatus(const Async::AsyncStatus status) noexcept {
    switch (status) {
    case Async::AsyncStatus::kSuccess:
        return AllocationStatus::Success;
    case Async::AsyncStatus::kStaleGeneration:
        return AllocationStatus::GenerationMismatch;
    case Async::AsyncStatus::kTimeout:
        return AllocationStatus::Timeout;
    case Async::AsyncStatus::kBusyRetryExhausted:
    case Async::AsyncStatus::kAborted:
    case Async::AsyncStatus::kHardwareError:
    case Async::AsyncStatus::kLockCompareFail:
    case Async::AsyncStatus::kShortRead:
        return AllocationStatus::Failed;
    }
    return AllocationStatus::Failed;
}

uint64_t IRMClient::CurrentMonotonicNowNs() noexcept {
#ifdef ASFW_HOST_TEST
    return ASFW::Testing::HostMonotonicNow();
#else
    static mach_timebase_info_data_t timebase{};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    const uint64_t ticks = mach_absolute_time();
    return (ticks * timebase.numer) / timebase.denom;
#endif
}

void IRMClient::DelayForPostResetQuietPeriod() const {
    constexpr uint64_t kQuietPeriodNs = 1'000'000'000ULL;

    if (lastBusResetNs_ == 0) {
        return;
    }

    const uint64_t nowNs = CurrentMonotonicNowNs();
    if (nowNs <= lastBusResetNs_) {
        return;
    }

    const uint64_t elapsedNs = nowNs - lastBusResetNs_;
    if (elapsedNs >= kQuietPeriodNs) {
        return;
    }

    const uint64_t remainingMs = (kQuietPeriodNs - elapsedNs + 999'999ULL) / 1'000'000ULL;
    ASFW_LOG(IRM, "IRMClient: waiting %llums for post-reset quiet period", remainingMs);
    IOSleep(static_cast<unsigned int>(remainingMs));
}

void IRMClient::ReadIRMQuadlet(
    uint32_t addressLo,
    std::function<void(AllocationStatus status, uint32_t value)> callback)
{
    auto callbackState = Common::ShareCallback(std::move(callback));
    Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = IRMRegisters::kAddressHi,
        .addressLo = addressLo,
    }};

    FW::FwSpeed speed{0};
    FW::NodeId node{irmNodeId_};
    FW::Generation gen{generation_};

    bus_.ReadQuad(gen, node, addr, speed,
        [callbackState](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            const AllocationStatus mapped = IRMClient::MapAsyncStatus(status);
            if (mapped != AllocationStatus::Success) {
                Common::InvokeSharedCallback(callbackState, mapped, 0u);
                return;
            }

            if (payload.size() != 4) {
                Common::InvokeSharedCallback(callbackState, AllocationStatus::Failed, 0u);
                return;
            }

            uint32_t raw = 0;
            std::memcpy(&raw, payload.data(), sizeof(raw));
            const uint32_t hostValue = OSSwapBigToHostInt32(raw);
            Common::InvokeSharedCallback(callbackState, AllocationStatus::Success, hostValue);
        });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void IRMClient::CompareSwapIRMQuadlet(
    uint32_t addressLo, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t expected,
    uint32_t desired,
    std::function<void(AllocationStatus status, uint32_t oldValue)> callback)
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

    bus_.Lock(gen, node, addr, FW::LockOp::kCompareSwap,
        std::span{operand}, 4, speed,
        [callbackState](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            const AllocationStatus mapped = IRMClient::MapAsyncStatus(status);
            if (mapped != AllocationStatus::Success) {
                Common::InvokeSharedCallback(callbackState, mapped, 0u);
                return;
            }

            if (payload.size() != 4) {
                Common::InvokeSharedCallback(callbackState, AllocationStatus::Failed, 0u);
                return;
            }

            uint32_t raw = 0;
            std::memcpy(&raw, payload.data(), sizeof(raw));
            const uint32_t oldValue = OSSwapBigToHostInt32(raw);
            Common::InvokeSharedCallback(callbackState, AllocationStatus::Success, oldValue);
        });
}

void IRMClient::ReadIRMWindow(ResourceSnapshotCallback callback)
{
    if (irmNodeId_ == 0xFF) {
        callback(AllocationStatus::NotFound, {});
        return;
    }

    auto callbackState = Common::ShareCallback(std::move(callback));
    auto snapshot = std::make_shared<ResourceSnapshot>();

    ReadIRMQuadlet(IRMRegisters::kBandwidthAvailable,
        [this, callbackState, snapshot](AllocationStatus status, uint32_t bandwidthAvailable) {
            if (status != AllocationStatus::Success) {
                Common::InvokeSharedCallback(callbackState, status, ResourceSnapshot{});
                return;
            }

            snapshot->bandwidthAvailable = bandwidthAvailable;
            ReadIRMQuadlet(IRMRegisters::kChannelsAvailable31_0,
                [this, callbackState, snapshot](AllocationStatus status, uint32_t channelsAvailable31_0) {
                    if (status != AllocationStatus::Success) {
                        Common::InvokeSharedCallback(callbackState, status, ResourceSnapshot{});
                        return;
                    }

                    snapshot->channelsAvailable31_0 = channelsAvailable31_0;
                    ReadIRMQuadlet(IRMRegisters::kChannelsAvailable63_32,
                        [callbackState, snapshot](AllocationStatus status, uint32_t channelsAvailable63_32) {
                            if (status != AllocationStatus::Success) {
                                Common::InvokeSharedCallback(callbackState, status, ResourceSnapshot{});
                                return;
                            }

                            snapshot->channelsAvailable63_32 = channelsAvailable63_32;
                            Common::InvokeSharedCallback(callbackState, AllocationStatus::Success, *snapshot);
                        });
                });
        });
}

void IRMClient::SetIRMNode(uint8_t irmNodeId, Generation generation, uint64_t lastBusResetNs) {
    irmNodeId_ = irmNodeId;
    generation_ = generation;
    lastBusResetNs_ = lastBusResetNs;

    ASFW_LOG(IRM, "IRMClient: Set IRM node=%u generation=%u resetNs=%llu",
             irmNodeId, generation.value, lastBusResetNs);
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
        uint8_t retriesLeft{0};
    };

    auto ctx = std::make_shared<ResourceContext>(ResourceContext{
        channel, bandwidthUnits, retryPolicy, std::move(callback), retryPolicy.maxRetries
    });

    if (ctx->channel >= 64) {
        ctx->userCallback(AllocationStatus::Failed);
        return;
    }
    if (irmNodeId_ == 0xFF) {
        ctx->userCallback(AllocationStatus::NotFound);
        return;
    }

    DelayForPostResetQuietPeriod();

    auto attempt = std::make_shared<std::function<void()>>();
    *attempt = [this, ctx, attempt]() {
        ReadIRMWindow([this, ctx, attempt](AllocationStatus snapshotStatus, ResourceSnapshot snapshot) {
            if (snapshotStatus != AllocationStatus::Success) {
                ctx->userCallback(snapshotStatus);
                return;
            }

            if (snapshot.bandwidthAvailable < ctx->bandwidthUnits) {
                ctx->userCallback(AllocationStatus::NoResources);
                return;
            }

            const uint32_t currentChannels =
                (ctx->channel < 32) ? snapshot.channelsAvailable31_0 : snapshot.channelsAvailable63_32;
            const uint32_t bitMask = ChannelToBitMask(ctx->channel);
            if ((currentChannels & bitMask) == 0) {
                ctx->userCallback(AllocationStatus::NoResources);
                return;
            }

            const uint32_t desiredBandwidth = snapshot.bandwidthAvailable - ctx->bandwidthUnits;
            CompareSwapBandwidth(
                snapshot.bandwidthAvailable,
                desiredBandwidth,
                [this, ctx, snapshot, currentChannels, bitMask, attempt](AllocationStatus bwStatus,
                                                                         uint32_t oldBandwidth) mutable {
                    if (bwStatus != AllocationStatus::Success) {
                        const bool retryable =
                            (bwStatus == AllocationStatus::NoResources) &&
                            (oldBandwidth >= ctx->bandwidthUnits) &&
                            (ctx->retriesLeft > 0);
                        if (retryable) {
                            --ctx->retriesLeft;
                            (*attempt)();
                            return;
                        }
                        ctx->userCallback(bwStatus);
                        return;
                    }

                    const uint32_t desiredChannels = currentChannels & ~bitMask;
                    CompareSwapChannel(
                        ctx->channel,
                        currentChannels,
                        desiredChannels,
                        [this, ctx, bitMask, attempt](AllocationStatus channelStatus,
                                                      uint32_t oldChannels) mutable {
                            if (channelStatus == AllocationStatus::Success) {
                                ctx->userCallback(AllocationStatus::Success);
                                return;
                            }

                            ReleaseBandwidth(
                                ctx->bandwidthUnits,
                                [ctx, bitMask, attempt, channelStatus, oldChannels](AllocationStatus) mutable {
                                    const bool retryable =
                                        (channelStatus == AllocationStatus::NoResources) &&
                                        ((oldChannels & bitMask) != 0) &&
                                        (ctx->retriesLeft > 0);
                                    if (retryable) {
                                        --ctx->retriesLeft;
                                        (*attempt)();
                                        return;
                                    }
                                    ctx->userCallback(channelStatus);
                                },
                                ctx->retryPolicy);
                        });
                });
        });
    };

    (*attempt)();
}

void IRMClient::ReadResourcesSnapshot(ResourceSnapshotCallback callback)
{
    ReadIRMWindow(std::move(callback));
}

void IRMClient::CompareSwapBandwidth(uint32_t expected,
                                     uint32_t desired,
                                     CompareSwapCallback callback)
{
    auto callbackState = Common::ShareCallback(std::move(callback));
    CompareSwapIRMQuadlet(IRMRegisters::kBandwidthAvailable,
                          expected,
                          desired,
                          [callbackState, expected](AllocationStatus status, uint32_t oldValue) {
                              if (status != AllocationStatus::Success) {
                                  Common::InvokeSharedCallback(callbackState, status, uint32_t{0});
                                  return;
                              }

                              const auto result =
                                  (oldValue == expected) ? AllocationStatus::Success
                                                         : AllocationStatus::NoResources;
                              Common::InvokeSharedCallback(callbackState, result, oldValue);
                          });
}

void IRMClient::CompareSwapChannel(uint8_t channel,
                                   uint32_t expected,
                                   uint32_t desired,
                                   CompareSwapCallback callback)
{
    auto callbackState = Common::ShareCallback(std::move(callback));
    CompareSwapIRMQuadlet(ChannelToRegisterAddress(channel),
                          expected,
                          desired,
                          [callbackState, expected](AllocationStatus status, uint32_t oldValue) {
                              if (status != AllocationStatus::Success) {
                                  Common::InvokeSharedCallback(callbackState, status, uint32_t{0});
                                  return;
                              }

                              const auto result =
                                  (oldValue == expected) ? AllocationStatus::Success
                                                         : AllocationStatus::NoResources;
                              Common::InvokeSharedCallback(callbackState, result, oldValue);
                          });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void IRMClient::ReleaseResources(uint8_t channel,
                                 uint32_t bandwidthUnits,
                                 AllocationCallback callback,
                                 const RetryPolicy& retryPolicy)
{
    ReleaseBandwidth(bandwidthUnits,
        [this, channel, callback = std::move(callback), retryPolicy](AllocationStatus bandwidthStatus) mutable {
            if (bandwidthStatus != AllocationStatus::Success) {
                callback(bandwidthStatus);
                return;
            }

            ReleaseChannel(channel,
                [callback = std::move(callback)](AllocationStatus channelStatus) mutable {
                    callback(channelStatus);
                },
                retryPolicy);
        },
        retryPolicy);
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
    ReadIRMQuadlet(ctx->addressLo, [this, ctx](AllocationStatus status, uint32_t currentValue) {
        if (status != AllocationStatus::Success) {
            ctx->userCallback(status);
            return;
        }

        OnChannelRead(ctx, true, currentValue);
    });
}

void IRMClient::OnChannelRead(const std::shared_ptr<ChannelLockState>& ctx,
                              const bool success,
                              const uint32_t currentValue) {
    if (!success) {
        ASFW_LOG_ERROR(IRM, "Channel read failed");
        ctx->userCallback(AllocationStatus::Failed);
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
                          [this, ctx, currentValue](AllocationStatus status, uint32_t oldValue) {
                              if (status != AllocationStatus::Success) {
                                  ctx->userCallback(status);
                                  return;
                              }
                              OnChannelCompareSwap(ctx, currentValue, true, oldValue);
                          });
}

void IRMClient::OnChannelCompareSwap(const std::shared_ptr<ChannelLockState>& ctx,
                                     const uint32_t expectedValue,
                                     const bool success,
                                     const uint32_t oldValue) {
    if (!success) {
        ASFW_LOG_ERROR(IRM, "Channel lock operation failed");
        ctx->userCallback(AllocationStatus::Failed);
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
                   [this, ctx](AllocationStatus status, uint32_t currentBandwidth) {
                       if (status != AllocationStatus::Success) {
                           ctx->userCallback(status);
                           return;
                       }
                       OnBandwidthRead(ctx, true, currentBandwidth);
                   });
}

void IRMClient::OnBandwidthRead(const std::shared_ptr<BandwidthLockState>& ctx,
                                const bool success,
                                const uint32_t currentBandwidth) {
    if (!success) {
        ASFW_LOG_ERROR(IRM, "Bandwidth read failed");
        ctx->userCallback(AllocationStatus::Failed);
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
    } else if (currentBandwidth + ctx->units > kMaxBandwidthUnitsS400) {
        ASFW_LOG(IRM,
                 "Bandwidth release skipped (available=%u release=%u would exceed max=%u)",
                 currentBandwidth,
                 ctx->units,
                 kMaxBandwidthUnitsS400);
        ctx->userCallback(AllocationStatus::Success);
        return;
    }

    CompareSwapIRMQuadlet(IRMRegisters::kBandwidthAvailable, currentBandwidth, newBandwidth,
                          [this, ctx, currentBandwidth](AllocationStatus status, uint32_t oldValue) {
                              if (status != AllocationStatus::Success) {
                                  ctx->userCallback(status);
                                  return;
                              }
                              OnBandwidthCompareSwap(ctx, currentBandwidth, true, oldValue);
                          });
}

void IRMClient::OnBandwidthCompareSwap(const std::shared_ptr<BandwidthLockState>& ctx,
                                       const uint32_t expectedBandwidth,
                                       const bool success,
                                       const uint32_t oldValue) {
    if (!success) {
        ASFW_LOG_ERROR(IRM, "Bandwidth lock operation failed");
        ctx->userCallback(AllocationStatus::Failed);
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
