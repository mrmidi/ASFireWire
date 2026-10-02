// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../../Protocols/AVC/Discovery/DiscoveryOwner.hpp"
#include "../../Core/IAVCAudioConfigListener.hpp"
#include "../../../Protocols/BeBoB/Bootloader/BeBoBBootloaderPreparationCoordinator.hpp"
#include <map>
#include <optional>
#include <variant>
namespace ASFW::Audio::AVC {
struct Ready {};
struct WaitingForDiscovery {};
struct Failed { std::string reason; };
using PublicationState = std::variant<Ready, WaitingForDiscovery, Failed>;
// Exceptions are off: a variant that became valueless would abort.
static_assert(std::is_nothrow_move_constructible_v<PublicationState>);
/// Audio-owned publication, policy and preparation. Runs on the driver's work
/// queue; evaluation is triggered by discovery/extension completion, not timers.
class DiscoveryCoordinator final : public Protocols::AVC::DiscoveryOwner,
    public std::enable_shared_from_this<DiscoveryCoordinator> {
public:
    DiscoveryCoordinator(Discovery::DeviceRegistry& registry, Protocols::Ports::FireWireBusOps& bus,
                         Protocols::Ports::FireWireBusInfo& busInfo, IAVCAudioConfigListener* listener);
    void PrepareProducer(std::shared_ptr<Discovery::FWUnit> unit, std::function<void(bool)> ready) override;
    bool AllowsDiscovery(const Discovery::FWUnit& unit) const override;
    Protocols::AVC::AVCUnit::DiscoveryOptions OptionsFor(const Discovery::FWUnit& unit) const override;
    void UnitCreated(const std::shared_ptr<Protocols::AVC::AVCUnit>& unit) override;
    void UnitCompleted(const std::shared_ptr<Protocols::AVC::AVCUnit>& unit, bool success) override;
    void DeviceAdded(std::shared_ptr<Discovery::FWDevice> device) override;
    void Shutdown() override;
    [[nodiscard]] PublicationState Status(uint64_t guid) const;
private:
    void PrepareDevice(std::shared_ptr<Discovery::FWDevice> device, std::function<void(bool)> ready);
    void StartPreparation(const Discovery::DeviceRouteToken& route, uint32_t vendorId, uint32_t modelId,
                          std::vector<std::function<void(bool)>> waiters);
    void OnPrepared(const Discovery::DeviceRouteToken& route, const Protocols::BeBoB::Bootloader::PreparationState& state);
    void PublishProfile(const Discovery::FWDevice& device);
    void Publish(uint64_t guid, const Model::ASFWAudioDevice& config);
    void Fail(uint64_t guid, std::string reason);
    /// Bootloader preparation, one record per device incarnation. A new
    /// incarnation replaces the record. A new route of the same incarnation
    /// reuses a Ready/Failed result and re-checks after a cue.
    struct Preparation {
        enum class State { Running, Ready, AwaitingReenumeration, Failed };
        Discovery::DeviceRouteToken route; ///< Route of the run, or of the result.
        uint32_t vendorId{0}, modelId{0};
        State state{State::Running};
        std::optional<Discovery::DeviceRouteToken> newerRoute; ///< Seen while running.
        std::vector<std::function<void(bool)>> waiters;
    };
    Discovery::DeviceRegistry& registry_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    Protocols::BeBoB::Bootloader::BeBoBBootloaderPreparationCoordinator preparation_;
    IAVCAudioConfigListener* listener_;
    std::map<uint64_t, PublicationState> publication_;
    std::map<uint64_t, Preparation> preparations_;
    bool stopped_{false};
};
}
