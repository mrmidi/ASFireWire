// SPDX-License-Identifier: Apache-2.0

#include "ResolvedAudioStreamProfile.hpp"

#include <algorithm>

namespace ASFW::Audio::DriverKit {

void ResolvedAudioStreamProfile::Load(
    Devices::ResolvedAudioEndpointProfile profile) noexcept {
    profile_ = std::move(profile);
}

bool ResolvedAudioStreamProfile::IsValid() const noexcept {
    return static_cast<bool>(profile_.endpointId) &&
           static_cast<bool>(profile_.deviceInstanceId) &&
           profile_.supportedRateCount != 0 &&
           profile_.runtimeCaps.hostToDeviceStreamCount != 0 &&
           profile_.runtimeCaps.deviceToHostStreamCount != 0;
}

const Devices::ResolvedAudioEndpointProfile&
ResolvedAudioStreamProfile::Value() const noexcept { return profile_; }

Devices::ResolvedAudioEndpointProfile&
ResolvedAudioStreamProfile::MutableValue() noexcept { return profile_; }

const char* ResolvedAudioStreamProfile::Name() const noexcept {
    return profile_.deviceName.c_str();
}

Encoding::AudioWireFormat ResolvedAudioStreamProfile::TxWireFormat() const noexcept {
    return profile_.playbackWireFormat;
}

Encoding::AudioWireFormat ResolvedAudioStreamProfile::RxWireFormat() const noexcept {
    return profile_.captureWireFormat;
}

bool ResolvedAudioStreamProfile::BuildStream(
    bool playback, ASFW::Isoch::Audio::AudioStreamConfig& out) const noexcept {
    if (!IsValid()) return false;
    const auto& caps = profile_.runtimeCaps;
    const uint32_t count = playback ? caps.hostToDeviceStreamCount
                                    : caps.deviceToHostStreamCount;
    if (count == 0) return false;
    const auto& stream = playback ? caps.hostToDeviceStreams[0]
                                  : caps.deviceToHostStreams[0];
    if (stream.pcmChannels == 0 || stream.am824Slots < stream.pcmChannels ||
        stream.pcmChannels > UINT8_MAX || stream.am824Slots > UINT8_MAX) {
        return false;
    }
    out = {};
    out.direction = playback
        ? ASFW::Isoch::Audio::AudioStreamDirection::HostToDevice
        : ASFW::Isoch::Audio::AudioStreamDirection::DeviceToHost;
    out.sampleRate = profile_.currentSampleRateHz;
    out.streamMode = profile_.streamMode == Devices::StreamModePolicy::Blocking
        ? Encoding::StreamMode::kBlocking
        : Encoding::StreamMode::kNonBlocking;
    out.pcmChannels = static_cast<uint8_t>(stream.pcmChannels);
    out.dbs = static_cast<uint8_t>(stream.am824Slots);
    out.midiSlots = static_cast<uint8_t>(stream.am824Slots - stream.pcmChannels);
    out.framesPerDataPacket = playback ? profile_.playbackFramesPerDataPacket
                                       : profile_.captureFramesPerDataPacket;
    out.fdf = playback ? profile_.playbackFdf : profile_.captureFdf;
    out.fmt = playback ? profile_.playbackFmt : profile_.captureFmt;
    return out.framesPerDataPacket != 0;
}

bool ResolvedAudioStreamProfile::BuildDefaultTxStreamConfig(
    ASFW::Isoch::Audio::AudioStreamConfig& out) const noexcept {
    return BuildStream(true, out);
}

bool ResolvedAudioStreamProfile::BuildDefaultRxStreamConfig(
    ASFW::Isoch::Audio::AudioStreamConfig& out) const noexcept {
    return BuildStream(false, out);
}

uint32_t ResolvedAudioStreamProfile::TxStreamCount() const noexcept {
    return profile_.runtimeCaps.hostToDeviceStreamCount;
}

uint32_t ResolvedAudioStreamProfile::RxStreamCount() const noexcept {
    return profile_.runtimeCaps.deviceToHostStreamCount;
}

ASFW::Isoch::Audio::AudioStreamTxPolicy
ResolvedAudioStreamProfile::TxStreamPolicy() const noexcept {
    return {
        .hostToDevicePcmEncoding = profile_.playbackWireFormat,
        .variableDbs = profile_.txPacketPolicy.variableDbs,
        .defaultNonAudioSlotWord = profile_.txPacketPolicy.defaultNonAudioSlotWord,
        .initializeNonAudioSlots = profile_.txPacketPolicy.initializeNonAudioSlots,
        .preserveFdfInNoDataPackets = profile_.txPacketPolicy.preserveFdfInNoDataPackets,
        .emptyPacketsDuringIdle = profile_.txPacketPolicy.emptyPacketsDuringIdle,
        .advanceDbcOnNoData = profile_.txPacketPolicy.advanceDbcOnNoData,
    };
}

const Devices::RateTimingPolicy*
ResolvedAudioStreamProfile::Timing(double rate) const noexcept {
    return profile_.TimingFor(static_cast<uint32_t>(rate));
}

uint32_t ResolvedAudioStreamProfile::InitialClockAnchorTimeoutMs() const noexcept {
    const auto* timing = Timing(profile_.currentSampleRateHz);
    return timing ? timing->anchorTimeoutMs : 500U;
}

uint32_t ResolvedAudioStreamProfile::TxSafetyOffsetFrames(double rate) const noexcept {
    const auto* timing = Timing(rate);
    return timing ? timing->outputSafetyFrames : 0U;
}

uint32_t ResolvedAudioStreamProfile::RxSafetyOffsetFrames(double rate) const noexcept {
    const auto* timing = Timing(rate);
    return timing ? timing->inputSafetyFrames : 0U;
}

uint32_t ResolvedAudioStreamProfile::TxReportedLatencyFrames(double rate) const noexcept {
    const auto* timing = Timing(rate);
    return timing ? timing->outputLatencyFrames : 0U;
}

uint32_t ResolvedAudioStreamProfile::RxReportedLatencyFrames(double rate) const noexcept {
    const auto* timing = Timing(rate);
    return timing ? timing->inputLatencyFrames : 0U;
}

std::vector<uint32_t> ResolvedAudioStreamProfile::SupportedSampleRates() const {
    return {profile_.supportedRates.begin(),
            profile_.supportedRates.begin() + profile_.supportedRateCount};
}

uint32_t ResolvedAudioStreamProfile::RxTransferDelayTicks(double rate) const noexcept {
    const auto* timing = Timing(rate);
    return timing ? timing->rxTransferDelayTicks : 12800U;
}

uint32_t ResolvedAudioStreamProfile::TxTransferDelayTicks(double rate) const noexcept {
    const auto* timing = Timing(rate);
    return timing ? timing->txTransferDelayTicks : 12800U;
}

} // namespace ASFW::Audio::DriverKit
