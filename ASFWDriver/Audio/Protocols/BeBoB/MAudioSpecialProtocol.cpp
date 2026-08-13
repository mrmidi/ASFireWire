// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioSpecialProtocol.cpp — FireWire 1814 / ProjectMix I/O.
//
// Fresh implementation. Geometry and choreography are cross-validated with
// Linux sound/firewire/bebob/bebob_maudio.c; no reference source is copied.

#include "MAudioSpecialProtocol.hpp"

#include "../../../Logging/Logging.hpp"

namespace ASFW::Audio::BeBoB {

MAudioSpecialProtocol::MAudioSpecialProtocol(
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    Discovery::DeviceRouteToken route,
    IRM::IRMClient* irmClient,
    CMP::CMPClient* cmpClient,
    Scheduling::ITimerScheduler* timerScheduler,
    MAudioSpecialModel model) noexcept
    : BeBoBProtocol(busOps, busInfo, route, irmClient, cmpClient, timerScheduler),
      model_(model) {}

const char* MAudioSpecialProtocol::DeviceName() const {
    switch (model_) {
        case MAudioSpecialModel::FireWire1814:
            return "M-Audio FireWire 1814";
        case MAudioSpecialModel::ProjectMix:
            return "M-Audio ProjectMix I/O";
    }
    return "M-Audio BeBoB";
}

std::vector<uint32_t> MAudioSpecialProtocol::SupportedRates() const {
    // Same table for both, truncated for ProjectMix. Linux expresses this as
    // `max -= 2` over the shared formation loop rather than a second table
    // (bebob_maudio.c:241-243).
    const size_t count = model_ == MAudioSpecialModel::FireWire1814
                             ? kMAudioFireWire1814RateCount
                             : kMAudioProjectMixRateCount;
    return std::vector<uint32_t>(kMAudioSpecialRatesHz,
                                 kMAudioSpecialRatesHz + count);
}

AudioStreamRuntimeCaps MAudioSpecialProtocol::CapsForCurrentFormation() const noexcept {
    AudioStreamRuntimeCaps caps{};

    const auto formation =
        MAudioFormationFor(captureFormat_, playbackFormat_, currentRateHz_);
    if (!formation) {
        // Only reachable if currentRateHz_ was set to something SupportedRates()
        // never offered. Report empty rather than a plausible-looking shape: a
        // wrong geometry here is silent on the wire, and zero is not.
        ASFW_LOG_ERROR(Audio,
                       "[BeBoB] %{public}s: no formation for rate=%u — geometry unset",
                       DeviceName(), currentRateHz_);
        return caps;
    }

    // DBS is PCM plus the MIDI conformant-data block, in each direction
    // independently. bebob_maudio.c:248-253.
    const uint32_t captureSlots =
        formation->capturePcmChannels + formation->midiDataBlocks;
    const uint32_t playbackSlots =
        formation->playbackPcmChannels + formation->midiDataBlocks;

    // Aggregate (HAL) view.
    caps.hostInputPcmChannels = formation->capturePcmChannels;
    caps.hostOutputPcmChannels = formation->playbackPcmChannels;
    caps.deviceToHostAm824Slots = captureSlots;
    caps.hostToDeviceAm824Slots = playbackSlots;
    caps.sampleRateHz = currentRateHz_;
    caps.deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps.hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;

    // Per-stream (wire) view. Both must be filled: the aggregate view alone
    // leaves the transport with no stream to arm and StartIO fails.
    caps.deviceToHostStreamCount = 1;
    caps.hostToDeviceStreamCount = 1;
    caps.deviceToHostStreams[0] = {
        .pcmChannels = static_cast<uint16_t>(formation->capturePcmChannels),
        .am824Slots = static_cast<uint16_t>(captureSlots)};
    caps.hostToDeviceStreams[0] = {
        .pcmChannels = static_cast<uint16_t>(formation->playbackPcmChannels),
        .am824Slots = static_cast<uint16_t>(playbackSlots)};
    return caps;
}

AudioStreamRuntimeCaps MAudioSpecialProtocol::DeviceCaps() const {
    return CapsForCurrentFormation();
}

bool MAudioSpecialProtocol::GetRuntimeAudioStreamCaps(
    AudioStreamRuntimeCaps& outCaps) const {
    outCaps = CapsForCurrentFormation();
    return outCaps.sampleRateHz != 0;
}

void MAudioSpecialProtocol::ConfirmDuplexStart(ConfirmCallback callback) {
    // The inverted start choreography (H8).
    //
    // Every other BeBoB device here is fully configured before CMP: signal
    // format, then settle, then connect, then run. This firmware needs the rate
    // asserted *again* once host DMA is already running, and Linux is explicit
    // that this is what makes it transmit rather than a defensive re-send:
    //
    //   "The firmware customized by M-Audio uses these commands to start
    //    transmitting stream. This is not usual way."
    //     bebob_stream.c:648-659, after amdtp_domain_start()
    //
    // The coordinator has started host transmit before this stage runs
    // (kStartingHostTransmit precedes ConfirmingDeviceStart), so this is the
    // earliest point that matches Linux's ordering without adding a stage.
    //
    // Base confirm runs first: re-sending the rate into a connection that never
    // came up would produce a misleading AV/C failure for what is really a CMP
    // problem.
    BeBoBProtocol::ConfirmDuplexStart(
        [this, callback = std::move(callback)](IOReturn status,
                                               DuplexConfirmResult result) mutable {
            if (status != kIOReturnSuccess) {
                callback(status, result);
                return;
            }

            const AudioClockConfig clock{.sampleRateHz = currentRateHz_};
            ASFW_LOG(Audio,
                     "[BeBoB] %{public}s: re-sending signal format after DMA start "
                     "(rate=%u, interlock=%ums)",
                     DeviceName(), currentRateHz_, SignalFormatInterlockMs());

            ProgramSignalFormat(clock, [callback = std::move(callback), result](
                                           IOReturn fmtStatus) mutable {
                if (fmtStatus != kIOReturnSuccess) {
                    ASFW_LOG_ERROR(Audio,
                                   "[BeBoB] post-start signal format failed: 0x%08x — "
                                   "device will stay silent",
                                   fmtStatus);
                }
                callback(fmtStatus, result);
            });
        });
}

void MAudioSpecialProtocol::ReadClockHealth(HealthCallback callback) {
    // Deliberately does not interrogate the device. The one command that could
    // report a rate is the signal-format probe, and issuing FCP from a health
    // poll would put traffic on a freeze-prone device on a timer. Report what
    // we believe and let the caller see it is belief, not readback.
    callback(kIOReturnSuccess,
             DuplexHealthResult{.generation = busInfo_.GetGeneration(),
                                .appliedClock = appliedClock_,
                                .runtimeCaps = CapsForCurrentFormation(),
                                .sourceLocked = inputConnected_ && outputConnected_,
                                .clockReferenceHealthy = true,
                                .nominalRateHz = currentRateHz_});
}

} // namespace ASFW::Audio::BeBoB
