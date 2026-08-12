// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "AudioNubPublisher.hpp"
#include "../Devices/AudioDeviceSessionManager.hpp"
#include "../Duplex/AudioDuplexCoordinator.hpp"
#include "../Duplex/IsochDuplexHostTransport.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
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
    std::unordered_set<EndpointId, Devices::AudioEndpointIdHash>
        invalidatedEndpoints_{};
};

} // namespace ASFW::Audio
