// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MackieOnyxProtocol.cpp — Runtime duplex control for the Mackie Onyx-i (Oxford run).
//
// Fresh implementation on the shared AV/C+CMP base. Wire choreography is the
// base's, cross-validated against Linux sound/firewire/bebob/bebob_stream.c;
// the geometry below is the live Onyx 820i capture (2026-08-17): capture 8ch
// MBLA, playback 2ch MBLA, compound AM824, one isoch plug per direction,
// current rate 44.1 kHz. No reference source is copied.

#include "MackieOnyxProtocol.hpp"

#include "../../../../Logging/Logging.hpp"

namespace ASFW::Audio::Oxford::Mackie {

MackieOnyxProtocol::MackieOnyxProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                       Protocols::Ports::FireWireBusInfo& busInfo,
                                       Discovery::DeviceRouteToken route,
                                       IRM::IRMClient* irmClient,
                                       CMP::CMPClient* cmpClient,
                                       Scheduling::ITimerScheduler* timerScheduler) noexcept
    : BeBoBProtocol(busOps, busInfo, route, irmClient, cmpClient, timerScheduler) {
    // Asymmetric duplex from the live capture: MBLA-only streams, so AM824
    // slots equal PCM channels in each direction. deviceToHost = host capture.
    constexpr uint16_t kCaptureChannels = 8;
    constexpr uint16_t kPlaybackChannels = 2;

    caps_.hostInputPcmChannels = kCaptureChannels;
    caps_.hostOutputPcmChannels = kPlaybackChannels;
    caps_.deviceToHostAm824Slots = kCaptureChannels;
    caps_.hostToDeviceAm824Slots = kPlaybackChannels;
    caps_.sampleRateHz = 44100U;
    caps_.deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps_.hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps_.deviceToHostStreamCount = 1;
    caps_.hostToDeviceStreamCount = 1;
    caps_.deviceToHostStreams[0] = {.pcmChannels = kCaptureChannels,
                                    .am824Slots = kCaptureChannels};
    caps_.hostToDeviceStreams[0] = {.pcmChannels = kPlaybackChannels,
                                    .am824Slots = kPlaybackChannels};
}

std::vector<uint32_t> MackieOnyxProtocol::SupportedRates() const {
    // 44.1 kHz only, deliberately, until the ADK transport reconfiguration
    // supports rate changes for AV/C static-profile devices. Field-verified
    // regression 2026-08-17: with 48 kHz in this list, a FAILED ADK rate
    // change (HandleChangeSampleRate -> transport reconfig kIOReturnUnsupported)
    // leaves session.pendingClock=48000 behind (RequestClockConfig persists it
    // before execution and the failure path does not scrub it), and the next
    // session start then programs the device to 48 kHz via SIGNAL FORMAT while
    // the host graph runs 44.1 -> audible periodic dropouts. Keeping 48 kHz
    // out of this list makes ApplyClockConfig refuse the stale pending clock.
    // The device itself supports 44.1/48/88.2/96 kHz (AV/C EXTENDED STREAM
    // FORMAT capture); the 0x18/0x19 transition machinery in the base is ready
    // when the reconfig path lands.
    return {44100U};
}

void MackieOnyxProtocol::ReadClockHealth(HealthCallback callback) {
    // The OXFW971 exposes no readable clock-status register and is SYT-unaware
    // (Linux snd-oxfw: CIP_UNAWARE_SYT), so PCR connectivity is the health
    // authority — same policy as the generic BeBoB adapter.
    callback(kIOReturnSuccess,
             DuplexHealthResult{.generation = busInfo_.GetGeneration(),
                                .appliedClock = appliedClock_,
                                .runtimeCaps = caps_,
                                .sourceLocked = inputConnected_ && outputConnected_,
                                .clockReferenceHealthy = true,
                                .nominalRateHz = caps_.sampleRateHz});
}

} // namespace ASFW::Audio::Oxford::Mackie
