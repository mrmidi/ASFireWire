// SPDX-License-Identifier: Apache-2.0
#include "BeBoBBootloaderPreparationCoordinator.hpp"
#include "../../../Logging/Logging.hpp"

#include <algorithm>
#include <atomic>
#include <memory>

namespace ASFW::Protocols::BeBoB::Bootloader {
namespace {

class PreparationRun final : public std::enable_shared_from_this<PreparationRun> {
public:
    PreparationRun(Async::IFireWireBusOps& bus,
                   Discovery::DeviceRegistry& registry,
                   Discovery::DeviceRouteToken route,
                   FW::FwSpeed speed,
                   std::function<bool()> ownerAlive)
        : bus_(bus), registry_(registry), route_(route), speed_(speed),
          ownerAlive_(std::move(ownerAlive)) {}

    void Start() { ReadInfo(); }

private:
    void ReadInfo()
    {
        if (!ownerAlive_()) return;
        if (!registry_.IsCurrent(route_)) {
            Retire(GenerationInvalidated{});
            return;
        }
        const auto self = shared_from_this();
        const auto completed = std::make_shared<std::atomic<bool>>(false);
        const auto handle = bus_.ReadBlock(
            route_.generation, FW::NodeId{static_cast<uint8_t>(route_.nodeId)},
            Async::FWAddress{Async::FWAddress::AddressParts{kAddressHi, kInfoAddressLo}},
            static_cast<uint32_t>(kInfoBlockBytes), speed_,
            [self, completed](Async::AsyncStatus status,
                              std::span<const uint8_t> payload) {
                if (completed->exchange(true, std::memory_order_acq_rel)) return;
                if (!self->ownerAlive_()) return;
                if (!self->registry_.IsCurrent(self->route_)) {
                    self->Retire(GenerationInvalidated{});
                    return;
                }
                if (status != Async::AsyncStatus::kSuccess ||
                    payload.size() < kInfoBlockBytes) {
                    self->Advance(InfoReadFailed{});
                    return;
                }
                BootRomInfo info{};
                std::copy_n(payload.begin(), info.raw.size(), info.raw.begin());
                self->Advance(InfoReadSucceeded{info});
            });
        if (!handle.IsValid() && !completed->exchange(true, std::memory_order_acq_rel)) {
            Advance(InfoReadFailed{});
        }
    }

    void Advance(const PreparationEvent& event)
    {
        auto step = AdvancePreparation(state_, event);
        state_ = std::move(step.state);
        if (std::holds_alternative<CueWriteSucceeded>(event)) {
            ASFW_LOG(AVC,
                     "[BootloaderCue] cue written GUID=0x%016llx gen=%u; awaiting re-enumeration",
                     route_.guid, route_.generation.value);
        }
        LogIfRetired();
        if (std::holds_alternative<ReadInfoBlock>(step.action)) {
            ReadInfo();
        } else if (const auto* cue = std::get_if<WriteCue>(&step.action)) {
            ASFW_LOG(AVC,
                     "[BootloaderCue] loader active; writing start-firmware cue "
                     "GUID=0x%016llx gen=%u protocol=%u",
                     route_.guid, route_.generation.value, cue->cue.ProtocolVersion());
            Write(*cue);
        }
    }

    void Retire(const PreparationEvent& event)
    {
        auto step = AdvancePreparation(state_, event);
        state_ = std::move(step.state);
        LogIfRetired();
    }

    // Bring-up path, at most a few records per device incarnation. Without
    // them the ring cannot distinguish "cue ran" from "cue never started".
    void LogIfRetired() noexcept
    {
        const auto* retired = std::get_if<Retired>(&state_);
        if (retired == nullptr || retireLogged_) return;
        retireLogged_ = true;
        ASFW_LOG(AVC, "[BootloaderCue] retired GUID=0x%016llx gen=%u reason=%{public}s",
                 route_.guid, route_.generation.value, RetireReasonName(retired->reason));
    }

    void Write(const WriteCue& action)
    {
        if (!ownerAlive_()) return;
        if (!registry_.IsCurrent(route_)) {
            Retire(GenerationInvalidated{});
            return;
        }
        const auto self = shared_from_this();
        const auto completed = std::make_shared<std::atomic<bool>>(false);
        const auto handle = bus_.WriteBlock(
            route_.generation, FW::NodeId{static_cast<uint8_t>(route_.nodeId)},
            Async::FWAddress{Async::FWAddress::AddressParts{kAddressHi, kRequestAddressLo}},
            action.cue.Bytes(), speed_,
            [self, completed](Async::AsyncStatus status, std::span<const uint8_t>) {
                if (completed->exchange(true, std::memory_order_acq_rel)) return;
                if (!self->ownerAlive_()) return;
                if (!self->registry_.IsCurrent(self->route_)) {
                    self->Retire(GenerationInvalidated{});
                    return;
                }
                self->Advance(status == Async::AsyncStatus::kSuccess
                                  ? PreparationEvent{CueWriteSucceeded{}}
                                  : PreparationEvent{CueWriteFailed{}});
            });
        if (!handle.IsValid() && !completed->exchange(true, std::memory_order_acq_rel)) {
            Advance(CueWriteFailed{});
        }
    }

    Async::IFireWireBusOps& bus_;
    Discovery::DeviceRegistry& registry_;
    const Discovery::DeviceRouteToken route_;
    const FW::FwSpeed speed_;
    std::function<bool()> ownerAlive_;
    PreparationState state_{BeginPreparation().state};
    bool retireLogged_{false};
};

} // namespace

BeBoBBootloaderPreparationCoordinator::BeBoBBootloaderPreparationCoordinator(
    Async::IFireWireBusOps& bus, Discovery::DeviceRegistry& registry) noexcept
    : bus_(bus), registry_(registry), lock_(IOLockAlloc()) {}

BeBoBBootloaderPreparationCoordinator::~BeBoBBootloaderPreparationCoordinator() {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

bool BeBoBBootloaderPreparationCoordinator::Prepare(
    const DeviceProfiles::Audio::StaticAudioEndpointPlan& plan,
    uint32_t vendorId, uint32_t modelId,
    const Discovery::DeviceRouteToken& route, FW::FwSpeed speed,
    std::function<bool()> ownerAlive) {
    if (!lock_ || !ShouldPrepareBootloader(plan, vendorId, modelId) || !ownerAlive ||
        !registry_.IsCurrent(route)) {
        return false;
    }
    IOLockLock(lock_);
    const bool firstAttempt =
        attemptsByIncarnation_.emplace(route.guid, route.deviceIncarnation).second;
    IOLockUnlock(lock_);
    if (!firstAttempt) {
        return false;
    }
    auto run = std::make_shared<PreparationRun>(bus_, registry_, route, speed,
                                               std::move(ownerAlive));
    run->Start();
    return true;
}

} // namespace ASFW::Protocols::BeBoB::Bootloader
