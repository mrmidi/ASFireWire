#pragma once

#include "FireWireBusPort.hpp"
#include "../../Common/WireFormat.hpp"
#include "../../Discovery/DeviceRegistry.hpp"

#include <DriverKit/IOReturn.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace ASFW::Protocols::Ports {

[[nodiscard]] constexpr IOReturn MapAsyncStatusToIOReturn(Async::AsyncStatus status) noexcept {
    switch (status) {
    case Async::AsyncStatus::kSuccess:
        return kIOReturnSuccess;
    case Async::AsyncStatus::kTimeout:
        return kIOReturnTimeout;
    case Async::AsyncStatus::kShortRead:
        return kIOReturnUnderrun;
    case Async::AsyncStatus::kBusyRetryExhausted:
        return kIOReturnBusy;
    case Async::AsyncStatus::kAborted:
        return kIOReturnAborted;
    case Async::AsyncStatus::kHardwareError:
        return kIOReturnError;
    case Async::AsyncStatus::kLockCompareFail:
        return kIOReturnExclusiveAccess;
    case Async::AsyncStatus::kStaleGeneration:
        return kIOReturnOffline;
    }
    return kIOReturnError;
}

class ProtocolRegisterIO {
public:
    using QuadReadCallback = std::function<void(Async::AsyncStatus, uint32_t)>;
    using BlockReadCallback = std::function<void(Async::AsyncStatus, std::span<const uint8_t>)>;
    using WriteCallback = std::function<void(Async::AsyncStatus)>;
    using CompareSwap64Callback = std::function<void(Async::AsyncStatus, uint64_t)>;

    ProtocolRegisterIO(FireWireBusOps& busOps,
                       FireWireBusInfo& busInfo,
                       Discovery::DeviceRegistry& routeRegistry,
                       const Discovery::DeviceRouteToken& route)
        : busOps_(busOps)
        , busInfo_(busInfo)
        , routeRegistry_(routeRegistry)
        , route_(route) {}

    [[nodiscard]] Async::AsyncHandle ReadQuadBE(
        Async::FWAddress address,
        QuadReadCallback callback,
        std::optional<FW::FwSpeed> speedOverride = std::nullopt)
    {
        if (!IsRouteCurrent()) {
            callback(Async::AsyncStatus::kStaleGeneration, 0U);
            return {};
        }
        const auto route = route_;
        return busOps_.ReadQuad(route.generation,
                                NodeId(),
                                address,
                                ResolveSpeed(speedOverride),
                                [this, route, callback = std::move(callback)](Async::AsyncStatus status,
                                                                 std::span<const uint8_t> payload) mutable {
                                    if (!callback) {
                                        return;
                                    }
                                    if (!routeRegistry_.IsCurrent(route)) {
                                        callback(Async::AsyncStatus::kStaleGeneration, 0U);
                                        return;
                                    }
                                    if (status != Async::AsyncStatus::kSuccess) {
                                        callback(status, 0U);
                                        return;
                                    }
                                    if (payload.size() < sizeof(uint32_t)) {
                                        callback(Async::AsyncStatus::kShortRead, 0U);
                                        return;
                                    }
                                    callback(Async::AsyncStatus::kSuccess, FW::ReadBE32(payload.data()));
                                });
    }

    [[nodiscard]] Async::AsyncHandle WriteQuadBE(
        Async::FWAddress address,
        uint32_t value,
        WriteCallback callback,
        std::optional<FW::FwSpeed> speedOverride = std::nullopt)
    {
        if (!IsRouteCurrent()) {
            callback(Async::AsyncStatus::kStaleGeneration);
            return {};
        }
        std::array<uint8_t, sizeof(uint32_t)> bytes{};
        FW::WriteBE32(bytes.data(), value);
        const auto route = route_;
        return busOps_.WriteBlock(route.generation,
                                  NodeId(),
                                  address,
                                  std::span<const uint8_t>(bytes.data(), bytes.size()),
                                  ResolveSpeed(speedOverride),
                                  [this, route, callback = std::move(callback)](Async::AsyncStatus status,
                                                                   std::span<const uint8_t>) mutable {
                                      if (callback) {
                                          callback(routeRegistry_.IsCurrent(route)
                                                       ? status
                                                       : Async::AsyncStatus::kStaleGeneration);
                                      }
                                  });
    }

    [[nodiscard]] Async::AsyncHandle ReadBlock(
        Async::FWAddress address,
        uint32_t length,
        BlockReadCallback callback,
        std::optional<FW::FwSpeed> speedOverride = std::nullopt)
    {
        if (!IsRouteCurrent()) {
            callback(Async::AsyncStatus::kStaleGeneration, {});
            return {};
        }
        const auto route = route_;
        return busOps_.ReadBlock(route.generation,
                                 NodeId(),
                                 address,
                                 length,
                                 ResolveSpeed(speedOverride),
                                 [this, route, callback = std::move(callback), length](Async::AsyncStatus status,
                                                                          std::span<const uint8_t> payload) mutable {
                                     if (!callback) {
                                         return;
                                     }
                                     if (!routeRegistry_.IsCurrent(route)) {
                                         callback(Async::AsyncStatus::kStaleGeneration, {});
                                         return;
                                     }
                                     if (status != Async::AsyncStatus::kSuccess) {
                                         callback(status, {});
                                         return;
                                     }
                                     if (payload.size() < length) {
                                         callback(Async::AsyncStatus::kShortRead, payload);
                                         return;
                                     }
                                     callback(Async::AsyncStatus::kSuccess, payload);
                                 });
    }

    [[nodiscard]] Async::AsyncHandle WriteBlock(
        Async::FWAddress address,
        std::span<const uint8_t> payload,
        WriteCallback callback,
        std::optional<FW::FwSpeed> speedOverride = std::nullopt)
    {
        if (!IsRouteCurrent()) {
            callback(Async::AsyncStatus::kStaleGeneration);
            return {};
        }
        const auto route = route_;
        return busOps_.WriteBlock(route.generation,
                                  NodeId(),
                                  address,
                                  payload,
                                  ResolveSpeed(speedOverride),
                                  [this, route, callback = std::move(callback)](Async::AsyncStatus status,
                                                                   std::span<const uint8_t>) mutable {
                                      if (callback) {
                                          callback(routeRegistry_.IsCurrent(route)
                                                       ? status
                                                       : Async::AsyncStatus::kStaleGeneration);
                                      }
                                  });
    }

    [[nodiscard]] Async::AsyncHandle CompareSwap64BE(
        Async::FWAddress address,
        uint64_t expected,
        uint64_t desired,
        CompareSwap64Callback callback,
        std::optional<FW::FwSpeed> speedOverride = std::nullopt)
    {
        if (!IsRouteCurrent()) {
            callback(Async::AsyncStatus::kStaleGeneration, 0ULL);
            return {};
        }
        std::array<uint8_t, 16> operand{};
        FW::WriteBE64(operand.data(), expected);
        FW::WriteBE64(operand.data() + 8, desired);

        const auto route = route_;
        return busOps_.Lock(route.generation,
                            NodeId(),
                            address,
                            FW::LockOp::kCompareSwap,
                            std::span<const uint8_t>(operand.data(), operand.size()),
                            8,
                            ResolveSpeed(speedOverride),
                            [this, route, callback = std::move(callback)](Async::AsyncStatus status,
                                                             std::span<const uint8_t> payload) mutable {
                                if (!callback) {
                                    return;
                                }
                                if (!routeRegistry_.IsCurrent(route)) {
                                    callback(Async::AsyncStatus::kStaleGeneration, 0ULL);
                                    return;
                                }
                                if (status != Async::AsyncStatus::kSuccess) {
                                    callback(status, 0ULL);
                                    return;
                                }
                                if (payload.size() < sizeof(uint64_t)) {
                                    callback(Async::AsyncStatus::kShortRead, 0ULL);
                                    return;
                                }
                                callback(Async::AsyncStatus::kSuccess, FW::ReadBE64(payload.data()));
                            });
    }

    [[nodiscard]] FW::NodeId NodeId() const noexcept {
        return FW::NodeId{static_cast<uint8_t>(route_.nodeId)};
    }

    void UpdateRoute(const Discovery::DeviceRouteToken& route) noexcept {
        route_ = route;
    }

    [[nodiscard]] bool IsRouteCurrent() const noexcept {
        return static_cast<bool>(route_) && routeRegistry_.IsCurrent(route_);
    }

private:
    [[nodiscard]] FW::FwSpeed ResolveSpeed(std::optional<FW::FwSpeed> speedOverride) const {
        return speedOverride.value_or(busInfo_.GetSpeed(NodeId()));
    }

    FireWireBusOps& busOps_;
    FireWireBusInfo& busInfo_;
    Discovery::DeviceRegistry& routeRegistry_;
    Discovery::DeviceRouteToken route_{};
};

} // namespace ASFW::Protocols::Ports
