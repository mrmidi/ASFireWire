// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "AudioFamilyProvider.hpp"
#include "ResolvedProfileBuilder.hpp"
#include "../../Discovery/IDeviceManager.hpp"
#include "../Families/BeBoB/Bootloader/BeBoBBootloaderClient.hpp"

#include <DriverKit/IOLib.h>

#include <map>
#include <memory>
#include <optional>
#include <functional>
#include <vector>

namespace ASFW::Discovery {
class DeviceRegistry;
class FWDevice;
}

namespace ASFW::Audio::Devices {

enum class AudioSessionState : uint8_t {
    Observed,
    /// Firmware preparation, ahead of any audio resolution. Only devices whose
    /// catalog plan carries a bootloader cue policy enter here, and the state is
    /// terminal by design: a prepared device resets, re-enumerates under a
    /// different identity, and is resolved from scratch. Preparation never
    /// becomes an audio endpoint. See MAUDIO_BOOTLOADER_CUE_DESIGN.md §5.
    Preparing,
    StaticResolved,
    Probing,
    Ready,
    Streaming,
    Quiescing,
    Retired,
    Quarantined,
    Failed,
};

struct AudioDeviceSessionSnapshot final {
    AudioEndpointId endpointId{};
    Discovery::UnitInstanceId unitId{};
    AudioSessionState state{AudioSessionState::Observed};
    DeviceProfiles::Audio::AudioFamilyProviderId provider{
        DeviceProfiles::Audio::AudioFamilyProviderId::None};
    Discovery::QuarantineReason quarantineReason{Discovery::QuarantineReason::None};
    uint64_t probeEpoch{0};
    std::shared_ptr<const ResolvedAudioEndpointProfile> profile;
};

class IAudioSessionSink {
public:
    virtual ~IAudioSessionSink() = default;
    virtual void EndpointReady(
        std::shared_ptr<const ResolvedAudioEndpointProfile> profile,
        std::shared_ptr<IDeviceProtocol> protocolHold) noexcept = 0;
    virtual void QuiesceEndpoint(AudioEndpointId endpointId) noexcept = 0;
    virtual void InvalidateEndpointBindings(AudioEndpointId endpointId) noexcept = 0;
    virtual void TerminateEndpoint(AudioEndpointId endpointId) noexcept = 0;
};

class AudioDeviceSessionManager final : public Discovery::IDeviceObserver,
                                        public IDeviceRouteAccessor {
public:
    using CatalogResolver = std::function<std::expected<
        DeviceProfiles::Audio::StaticAudioEndpointPlan,
        DeviceProfiles::Audio::CatalogResolutionError>(
            const Discovery::DeviceRecord&,
            const Discovery::UnitIdentityEvidence&)>;

    /// `busOps` and `scheduler` are optional and used only for bootloader
    /// preparation. Without both, a device carrying a cue policy is recorded and
    /// left alone rather than half-prepared — no partial firmware path exists.
    AudioDeviceSessionManager(Discovery::IDeviceManager& devices,
                              Discovery::DeviceRegistry& routes,
                              IAudioSessionSink& sink,
                              CatalogResolver catalogResolver = {},
                              Async::IFireWireBusOps* busOps = nullptr,
                              Scheduling::ITimerScheduler* scheduler = nullptr) noexcept;
    ~AudioDeviceSessionManager() override;

    AudioDeviceSessionManager(const AudioDeviceSessionManager&) = delete;
    AudioDeviceSessionManager& operator=(const AudioDeviceSessionManager&) = delete;

    [[nodiscard]] bool RegisterProvider(std::unique_ptr<IAudioFamilyProvider> provider) noexcept;
    void Start();
    void Shutdown() noexcept;

    [[nodiscard]] std::optional<AudioDeviceSessionSnapshot>
    Snapshot(AudioEndpointId endpointId) const noexcept;
    [[nodiscard]] std::vector<AudioDeviceSessionSnapshot> SnapshotAll() const;
    [[nodiscard]] std::optional<AudioEndpointId>
    EndpointForUnit(Discovery::UnitInstanceId unitId) const noexcept;
    [[nodiscard]] bool UpdateStreamingState(AudioEndpointId endpointId,
                                            bool streaming) noexcept;

    [[nodiscard]] std::optional<Discovery::DeviceRouteToken>
    CurrentRoute(Discovery::DeviceInstanceId instanceId) const noexcept override;
    [[nodiscard]] bool IsCurrent(
        const Discovery::DeviceRouteToken& route) const noexcept override;

    void OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceRemoved(Discovery::DeviceInstanceId instanceId) override;

private:
    struct Session final {
        AudioEndpointId endpointId{};
        Discovery::UnitInstanceId unitId{};
        AudioSessionState state{AudioSessionState::Observed};
        DeviceProfiles::Audio::StaticAudioEndpointPlan staticPlan{};
        Discovery::QuarantineReason quarantineReason{Discovery::QuarantineReason::None};
        uint64_t probeEpoch{0};
        std::unique_ptr<IAudioDeviceAdapter> adapter;
        std::shared_ptr<const ResolvedAudioEndpointProfile> profile;
        /// Set only for sessions in Preparing. The client owns transport; the
        /// state machine below owns every ordering rule.
        std::shared_ptr<Families::BeBoB::Bootloader::BeBoBBootloaderClient>
            bootloaderClient;
        Families::BeBoB::Bootloader::PreparationState preparationState{
            Families::BeBoB::Bootloader::ReadingInfo{}};
    };

    void ReconcileDevice(const std::shared_ptr<Discovery::FWDevice>& device);
    /// Starts the cue state machine for a device whose plan carries a cue policy.
    /// Creates the session in Preparing; no adapter and no probe are involved.
    void BeginPreparation(AudioEndpointId endpointId,
                          Discovery::DeviceRouteToken route) noexcept;
    /// Performs one action from the machine and feeds the outcome back as an
    /// event. The only place a cue write is issued.
    void DrivePreparation(
        AudioEndpointId endpointId, uint64_t preparationEpoch,
        Families::BeBoB::Bootloader::PreparationStep step) noexcept;
    void BeginProbe(AudioEndpointId endpointId,
                    const Discovery::DeviceRecord& record) noexcept;
    void CompleteProbe(AudioEndpointId endpointId, uint64_t probeEpoch,
                       Discovery::DeviceRecord record,
                       std::expected<FamilyProbeFacts, ProbeError> result) noexcept;
    void RetireSession(AudioEndpointId endpointId, const char* reason) noexcept;
    void TransitionLocked(Session& session, AudioSessionState next,
                          const char* reason) noexcept;
    [[nodiscard]] AudioEndpointId AllocateEndpointIdLocked() noexcept;
    [[nodiscard]] Session* FindLocked(AudioEndpointId endpointId) noexcept;
    [[nodiscard]] const Session* FindLocked(AudioEndpointId endpointId) const noexcept;

    Discovery::IDeviceManager& devices_;
    Discovery::DeviceRegistry& routes_;
    IAudioSessionSink& sink_;
    mutable IOLock* lock_{nullptr};
    bool observing_{false};
    bool shuttingDown_{false};
    uint64_t nextEndpointId_{0};
    std::map<DeviceProfiles::Audio::AudioFamilyProviderId,
             std::unique_ptr<IAudioFamilyProvider>> providers_;
    std::map<AudioEndpointId, Session> sessions_;
    std::map<Discovery::UnitInstanceId, AudioEndpointId> endpointByUnit_;
    CatalogResolver catalogResolver_;
    Async::IFireWireBusOps* busOps_{nullptr};
    Scheduling::ITimerScheduler* scheduler_{nullptr};
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};
};

} // namespace ASFW::Audio::Devices
