// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RMEFireface800Protocol.hpp - Stage 5H bounded circular silent playback integration.

#pragma once

#include "../IDeviceProtocol.hpp"
#include "../../../Protocols/Ports/FireWireBusPort.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio::RME {

class RMEFireface800Protocol final : public IDeviceProtocol {
public:
    RMEFireface800Protocol(Protocols::Ports::FireWireBusOps& busOps,
                           Protocols::Ports::FireWireBusInfo& busInfo,
                           uint16_t nodeId,
                           uint64_t deviceGuid,
                           ::ASFW::IRM::IRMClient* irmClient) noexcept;
    ~RMEFireface800Protocol() override;

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "RME Fireface 800 (Stage 5H bounded circular silent playback integration)"; }

    void UpdateRuntimeContext(uint16_t nodeId,
                              Protocols::AVC::FCPTransport* transport) override;
    bool GetPlaybackPreflightRoute(PlaybackPreflightRoute& outRoute) const override;
    IOReturn RunBoundedPlaybackIntegrationPreflight(
        const PlaybackPreflightRoute& route,
        BoundedPlaybackStep prepareHost,
        BoundedPlaybackStep runHostBurst,
        BoundedPlaybackStep cleanupHost) override;

private:
    void ProbeClockConfig() noexcept;
    void ValidateStfWritePath(uint32_t rate) noexcept;
    void VerifyClockAfterStfWrite(uint32_t expectedRate) noexcept;
    void ProbeSyncStatus() noexcept;
    void ProbeTxIsochChannel() noexcept;
    void ReservePlaybackResourcesPreflight(uint32_t rate) noexcept;
    void ProgramRxPacketFormat(uint32_t rate, uint8_t channel, uint32_t bandwidthUnits) noexcept;
    void RequestDeviceTxAllocation(uint32_t rate) noexcept;
    void PollDeviceTxIsochChannel(uint32_t rate, uint32_t attempt) noexcept;
    void StartDeviceCommunicationPreflight(uint32_t rate) noexcept;
    void StopDeviceCommunicationPreflight() noexcept;
    void RunNoCipWireFormatSelfTest() noexcept;
    void BestEffortStopDeviceCommunication() noexcept;
    void ReleaseReservedResources() noexcept;
    void LogStreamCaps(uint32_t rate) const noexcept;

    static uint32_t ReadLE32(const uint8_t* bytes) noexcept;
    static uint32_t DecodeSampleRate(uint32_t value) noexcept;
    static const char* DecodeClockSource(uint32_t value) noexcept;
    static const char* DecodeExternalState(uint32_t value,
                                           uint32_t lockedMask,
                                           uint32_t syncedMask) noexcept;
    static const char* DecodeReferredClockSource(uint32_t status0,
                                                  uint32_t status1) noexcept;
    static uint32_t DecodeReferredRate(uint32_t status0) noexcept;
    static const char* DecodeStreamMode(uint32_t rate) noexcept;
    static uint32_t DecodeChannelCount(uint32_t rate) noexcept;
    static uint32_t DecodeSytInterval(uint32_t rate) noexcept;
    static uint32_t CalculatePlaybackBandwidthUnits(uint32_t rate) noexcept;
    static uint8_t SelectAvailableChannel(uint32_t channels31_0,
                                          uint32_t channels63_32) noexcept;

    Protocols::Ports::FireWireBusOps& busOps_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    uint16_t nodeId_{0};
    uint64_t deviceGuid_{0};
    ::ASFW::IRM::IRMClient* irmClient_{nullptr};
    Async::AsyncHandle probeHandle_{};
    std::atomic<bool> active_{false};
    std::atomic<uint64_t> playbackPreflightRoute_{0U};
    uint32_t currentRate_{0};
    uint8_t reservedPlaybackChannel_{0xFF};
    uint32_t reservedPlaybackBandwidthUnits_{0};
    bool playbackResourcesReserved_{false};
    bool deviceTxAllocationRequested_{false};
    bool deviceTxAllocated_{false};
    uint8_t deviceTxChannel_{0xFF};
    std::atomic<bool> stage5hInFlight_{false};
};

} // namespace ASFW::Audio::RME
