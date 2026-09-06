// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../Shared/AudioRuntimeTuning.hpp"
#include "../../UserClient/WireFormats/AudioTuningWireFormats.hpp"
#include "AudioNubPublisher.hpp"
#include "../Devices/AudioDeviceSessionManager.hpp"
#include "../Duplex/AudioDuplexCoordinator.hpp"
#include "../Duplex/IsochDuplexHostTransport.hpp"
#include "../Protocols/Configuration/IAudioConfigurationControl.hpp"
#include "../Shared/Controls/IAudioControlSurface.hpp"
#include "../Shared/Topology/IAudioSemanticTopology.hpp"
#include "../Shared/Topology/IAudioSemanticConsoleLayout.hpp"
#include "../Shared/Topology/IAudioSemanticMatrix.hpp"
#include "../Shared/Metering/IAudioMetering.hpp"
#include "../Shared/Configuration/DeviceConfigurationSnapshot.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <array>
#include <functional>
#include <optional>
#include <unordered_set>

class IOService;

namespace ASFW::Audio {

class AudioRuntimeRegistry;

// Audio-side composition root. Discovery is observed exclusively by
// AudioDeviceSessionManager; this object receives resolved endpoint lifecycle
// events and owns the neutral duplex transport plus nub publication.
class AudioCoordinator final : public Devices::IAudioSessionSink {
public:
    using EndpointId = Devices::AudioEndpointId;

    AudioCoordinator(IOService* driver,
                     Discovery::DeviceRegistry& registry,
                     AudioRuntimeRegistry& runtime,
                     Driver::IsochService& isoch,
                     Driver::HardwareInterface& hardware) noexcept;
    ~AudioCoordinator() noexcept override;

    AudioCoordinator(const AudioCoordinator&) = delete;
    AudioCoordinator& operator=(const AudioCoordinator&) = delete;

    void SetTxPreparationCallback(
        Driver::IsochService::TxPreparationCallback callback) noexcept;
    void SetClockAnchorReadyCallback(
        IsochDuplexHostTransport::ClockAnchorReadyCallback callback) noexcept;
    using SessionStreamingCallback =
        std::function<bool(EndpointId endpointId, bool streaming)>;

    // Installed by ASFWAudioNub. Fires an OSAction so the blocking recovery
    // runs on ASFWAudioDriver's queue; see HandleTxTransportFault.
    using TxTransportFaultDispatch =
        std::function<void(uint32_t statusRaw, uint64_t streamGeneration)>;
    void SetTxTransportFaultDispatch(TxTransportFaultDispatch dispatch) noexcept;
    // Called by ASFWAudioNub from ASFWAudioDriver's queue once the OSAction hop
    // has landed. Blocks in RecoverStreaming(); never call it from the isoch
    // poll thread — that is the whole point of the hop.
    void RecoverAfterTxTransportFault(EndpointId endpointId) noexcept;
    void SetSessionStreamingCallback(SessionStreamingCallback callback) noexcept;

    // IAudioSessionSink. Calls arrive in strict session-manager teardown order.
    void EndpointReady(
        std::shared_ptr<const Devices::ResolvedAudioEndpointProfile> profile,
        std::shared_ptr<IDeviceProtocol> protocolHold) noexcept override;
    void QuiesceEndpoint(EndpointId endpointId) noexcept override;
    void InvalidateEndpointBindings(EndpointId endpointId) noexcept override;
    void TerminateEndpoint(EndpointId endpointId) noexcept override;

    void HandleCycleInconsistent() noexcept;
    [[nodiscard]] IOReturn StartStreaming(EndpointId endpointId) noexcept;
    [[nodiscard]] IOReturn StopStreaming(EndpointId endpointId) noexcept;
    [[nodiscard]] IOReturn RequestClockConfig(
        EndpointId endpointId,
        const AudioClockConfig& desiredClock,
        DuplexRestartReason reason) noexcept;

    // Applies only the hardware side of a semantic configuration. The ADK
    // service owns the corresponding CoreAudio projection and invokes
    // CommitDeviceConfiguration only after that projection succeeds.
    [[nodiscard]] IOReturn ApplyDeviceConfiguration(
        EndpointId endpointId,
        const Configuration::DeviceConfiguration& desired,
        AudioConfigurationApplyResult& outResult) noexcept;
    [[nodiscard]] IOReturn CommitDeviceConfiguration(
        EndpointId endpointId,
        const AudioConfigurationApplyResult& confirmed) noexcept;
    [[nodiscard]] IOReturn RequestDeviceConfiguration(
        EndpointId endpointId,
        const Configuration::DeviceConfiguration& desired) noexcept;

    // Diagnostic geometry sweep. Validated here as well as app-side: the app
    // cannot be trusted to be the same build as the driver, and installing a
    // geometry outside the compile-time envelope is worse than refusing it.
    [[nodiscard]] IOReturn RequestRuntimeTuning(
        EndpointId endpointId,
        const Shared::AudioRuntimeTuning& candidate,
        uint32_t groups) noexcept;
    [[nodiscard]] IOReturn CopyRuntimeTuningSnapshot(
        EndpointId endpointId,
        UserClient::Wire::AudioTuningSnapshotWire& out) noexcept;
    [[nodiscard]] IOReturn CopyDeviceConfigurationSnapshot(
        EndpointId endpointId,
        Configuration::DeviceConfigurationSnapshot& outSnapshot) noexcept;
    [[nodiscard]] uint32_t CopyConfigurationEndpointIds(
        std::array<EndpointId,
                   Configuration::kMaxConfigurationSnapshotCapabilities>& out) noexcept;
    [[nodiscard]] uint32_t CopySemanticTopologyEndpointIds(
        std::array<EndpointId, kMaxAudioSemanticTopologyEndpoints>& out) noexcept;
    [[nodiscard]] uint32_t CopySemanticMatrixEndpointIds(
        std::array<EndpointId, kMaxAudioSemanticMatrixEndpoints>& out) noexcept;
    [[nodiscard]] IOReturn CopyAudioControlSurfaceSnapshot(
        EndpointId endpointId, AudioControlSurfaceSnapshot& outSnapshot) noexcept;
    [[nodiscard]] IOReturn CopyAudioSemanticTopology(
        EndpointId endpointId, AudioSemanticTopologySnapshot& outSnapshot) noexcept;
    [[nodiscard]] IOReturn CopyAudioSemanticConsoleLayout(
        EndpointId endpointId, AudioSemanticConsoleLayoutSnapshot& outSnapshot) noexcept;
    [[nodiscard]] IOReturn CopyAudioSemanticMatrix(
        EndpointId endpointId, AudioSemanticMatrixSnapshot& outSnapshot) noexcept;
    [[nodiscard]] IOReturn RequestAudioControlValue(
        EndpointId endpointId, uint32_t controlId, int32_t value) noexcept;
    /// Starts a bounded semantic control write and returns immediately. The
    /// completion is the only authority for the resulting hardware belief.
    /// UserClient callers must use this instead of waiting on an async bus
    /// response on an external-method queue.
    [[nodiscard]] IOReturn SubmitAudioControlValue(
        EndpointId endpointId, uint32_t controlId, int32_t value,
        IAudioControlSurface::ApplyCallback completion) noexcept;
    [[nodiscard]] IOReturn CopyAudioMeterSnapshot(
        EndpointId endpointId, AudioMeterSnapshot& outSnapshot) noexcept;
    [[nodiscard]] IOReturn SetAudioMeteringEnabled(
        EndpointId endpointId, bool enabled) noexcept;
    void BeginTeardown() noexcept;

    [[nodiscard]] ASFWAudioNub* GetNub(EndpointId endpointId) const noexcept {
        return publisher_.GetNub(endpointId);
    }
    [[nodiscard]] std::optional<EndpointId>
    GetSinglePublishedEndpointId() const noexcept;

private:
    [[nodiscard]] kern_return_t StopHostTransport(
        const char* reason, bool generationInvalidated = false) noexcept;
    void HandleHostTimingLoss(EndpointId endpointId) noexcept;
    void HandleTxTransportFault(EndpointId endpointId, uint32_t statusRaw,
                                uint64_t streamGeneration) noexcept;
    [[nodiscard]] bool NotifySessionStreaming(EndpointId endpointId,
                                              bool streaming) noexcept;

    AudioNubPublisher publisher_;
    AudioRuntimeRegistry& runtime_;
    IsochDuplexHostTransport hostTransport_;
    std::atomic<bool> teardownRequested_{false};
    AudioDuplexCoordinator duplexCoordinator_;

    IOLock* lock_{nullptr};
    EndpointId activeEndpoint_{};
    SessionStreamingCallback sessionStreamingCallback_{};
    TxTransportFaultDispatch txTransportFaultDispatch_{};
    std::unordered_set<EndpointId, Devices::AudioEndpointIdHash>
        invalidatedEndpoints_{};
};

} // namespace ASFW::Audio
