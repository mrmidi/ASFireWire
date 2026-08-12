// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "BeBoBBootloaderClient.hpp"

#include "../../../../Discovery/DiscoveryTypes.hpp"
#include "../../../../Logging/Logging.hpp"

#include <algorithm>
#include <utility>

namespace ASFW::Audio::Families::BeBoB::Bootloader {

namespace {

/// The bootloader answers on its own schedule and this is a cold path, so the
/// request goes out at the conservative speed rather than the negotiated best.
constexpr ASFW::FW::FwSpeed kBootloaderSpeed = ASFW::FW::FwSpeed::S100;

constexpr uint64_t kNanosPerMilli = 1'000'000ULL;

} // namespace

BeBoBBootloaderClient::BeBoBBootloaderClient(
    ASFW::Async::IFireWireBusOps& busOps,
    ASFW::Scheduling::ITimerScheduler& scheduler,
    Discovery::DeviceRouteToken route) noexcept
    : busOps_(busOps), scheduler_(scheduler), route_(route) {}

std::optional<ASFW::FW::NodeId>
BeBoBBootloaderClient::OperationalNode() const noexcept {
    const auto node = Discovery::TryOperationalNodeId(route_.nodeId);
    if (!node) {
        return std::nullopt;
    }
    return ASFW::FW::NodeId{*node};
}

ASFW::Async::FWAddress BeBoBBootloaderClient::AddressFor(
    uint32_t addressLo) const noexcept {
    return ASFW::Async::FWAddress{ASFW::Async::FWAddress::QualifiedAddressParts{
        .addressHi = kBootloaderAddressHi,
        .addressLo = addressLo,
        .nodeID = route_.nodeId}};
}

void BeBoBBootloaderClient::ReadInfo(PreparationEventCallback completion) {
    if (cancelled_ || !completion) {
        return;
    }
    const auto node = OperationalNode();
    if (!node) {
        completion(InfoReadFailed{});
        return;
    }
    ASFW_LOG(Firmware, "[Bootloader] read info node=%u gen=%u bytes=%u",
             static_cast<unsigned>(route_.nodeId),
             static_cast<unsigned>(route_.generation.value), kInfoBlockBytes);

    (void)busOps_.ReadBlock(
        route_.generation, *node,
        AddressFor(kInfoAddressLo), kInfoBlockBytes, kBootloaderSpeed,
        [self = shared_from_this(), completion = std::move(completion)](
            ASFW::Async::AsyncStatus status,
            std::span<const uint8_t> payload) mutable {
            if (self->cancelled_) {
                return;
            }
            if (status != ASFW::Async::AsyncStatus::kSuccess ||
                payload.size() < kInfoBlockBytes) {
                ASFW_LOG(Firmware,
                         "[Bootloader] info read failed status=%{public}s bytes=%zu",
                         ASFW::Async::ToString(status), payload.size());
                completion(InfoReadFailed{});
                return;
            }
            BootRomInfo info{};
            std::copy_n(payload.begin(), kInfoBlockBytes, info.raw.begin());
            ASFW_LOG(Firmware,
                     "[Bootloader] info protocolVersion=%u bootloaderVersion=%u "
                     "active=%d",
                     info.ProtocolVersion(), info.BootloaderVersion(),
                     info.BootloaderActive() ? 1 : 0);
            completion(InfoReadSucceeded{info});
        });
}

void BeBoBBootloaderClient::SendCue(const BeBoBBootloaderCue& cue,
                                    PreparationEventCallback completion) {
    if (cancelled_ || !completion) {
        return;
    }
    const auto node = OperationalNode();
    if (!node) {
        completion(CueWriteFailed{});
        return;
    }
    const auto bytes = cue.Bytes();

    // The choke point. Reaching this with anything other than the frozen cue is
    // a programming error, and it is refused rather than transmitted: the
    // adjacent command codes rewrite device identity in flash permanently.
    if (!IsPermittedBootloaderWrite(kBootloaderAddressHi, kRequestAddressLo,
                                    bytes)) {
        ASFW_LOG_ERROR(Firmware,
                       "[Bootloader] refused a write that is not the frozen cue "
                       "- nothing was transmitted");
        completion(CueWriteFailed{});
        return;
    }

    ASFW_LOG(Firmware,
             "[Bootloader] cue node=%u gen=%u protocolVersion=%u",
             static_cast<unsigned>(route_.nodeId),
             static_cast<unsigned>(route_.generation.value),
             cue.ProtocolVersion());

    (void)busOps_.WriteBlock(
        route_.generation, *node,
        AddressFor(kRequestAddressLo), bytes, kBootloaderSpeed,
        [self = shared_from_this(), completion = std::move(completion)](
            ASFW::Async::AsyncStatus status,
            std::span<const uint8_t>) mutable {
            if (self->cancelled_) {
                return;
            }
            if (status != ASFW::Async::AsyncStatus::kSuccess) {
                // Reported as a failed write, which retires the machine. It is
                // never retried: a timed-out write may already have landed and
                // the device may be mid-boot.
                ASFW_LOG(Firmware, "[Bootloader] cue write failed status=%{public}s",
                         ASFW::Async::ToString(status));
                completion(CueWriteFailed{});
                return;
            }
            completion(CueWriteSucceeded{});
        });
}

void BeBoBBootloaderClient::ReadResponse(uint32_t delayMs,
                                         PreparationEventCallback completion) {
    if (cancelled_ || !completion) {
        return;
    }
    // A timer, never IOSleep — this runs on the single dispatch queue and
    // sleeping on it would stall every other subsystem.
    pendingTimer_ = scheduler_.ScheduleAfter(
        static_cast<uint64_t>(delayMs) * kNanosPerMilli,
        [self = shared_from_this(), completion = std::move(completion)]() mutable {
            if (self->cancelled_) {
                return;
            }
            self->pendingTimer_ = ASFW::Scheduling::kInvalidTimerToken;
            const auto node = self->OperationalNode();
            if (!node) {
                completion(ResponseReadFailed{});
                return;
            }
            (void)self->busOps_.ReadBlock(
                self->route_.generation, *node,
                self->AddressFor(kResponseAddressLo), sizeof(uint32_t),
                kBootloaderSpeed,
                [self, completion = std::move(completion)](
                    ASFW::Async::AsyncStatus status,
                    std::span<const uint8_t>) mutable {
                    if (self->cancelled_) {
                        return;
                    }
                    if (status != ASFW::Async::AsyncStatus::kSuccess) {
                        ASFW_LOG(Firmware,
                                 "[Bootloader] response read failed status=%{public}s",
                                 ASFW::Async::ToString(status));
                        completion(ResponseReadFailed{});
                        return;
                    }
                    ASFW_LOG(Firmware, "[Bootloader] response received");
                    completion(ResponseReadSucceeded{});
                });
        });
}

void BeBoBBootloaderClient::Cancel() noexcept {
    cancelled_ = true;
    if (pendingTimer_ != ASFW::Scheduling::kInvalidTimerToken) {
        scheduler_.Cancel(pendingTimer_);
        pendingTimer_ = ASFW::Scheduling::kInvalidTimerToken;
    }
}

} // namespace ASFW::Audio::Families::BeBoB::Bootloader
