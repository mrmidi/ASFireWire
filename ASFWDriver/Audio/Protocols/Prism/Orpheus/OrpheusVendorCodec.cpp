// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "OrpheusVendorCodec.hpp"
#include <cstring>

namespace ASFW::Audio::Prism::Orpheus {

std::vector<uint8_t> OrpheusVendorCodec::BuildFrame(uint8_t ctype,
                                                    OrpheusCommand command,
                                                    std::span<const uint8_t> operands,
                                                    size_t length) {
    if (length > 512 || Wire::kPayloadOffset + operands.size() > length) {
        return {};
    }
    std::vector<uint8_t> bytes(length, 0);
    bytes[0] = ctype;
    bytes[1] = Wire::kSubunitAudio;
    bytes[2] = Wire::kOpcodeVendorDependent;
    bytes[3] = Wire::kCompanyId[0];
    bytes[4] = Wire::kCompanyId[1];
    bytes[5] = Wire::kCompanyId[2];
    bytes[Wire::kCommandOffset] = static_cast<uint8_t>(command);
    for (size_t i = 0; i < operands.size(); ++i) {
        bytes[Wire::kPayloadOffset + i] = operands[i];
    }
    return bytes;
}

std::vector<uint8_t> OrpheusVendorCodec::BuildControlFrame(OrpheusCommand command,
                                                           std::span<const uint8_t> operands,
                                                           size_t length) {
    return BuildFrame(Wire::kCtypeControl, command, operands, length);
}

std::vector<uint8_t> OrpheusVendorCodec::BuildStatusFrame(OrpheusCommand command,
                                                          std::span<const uint8_t> operands,
                                                          size_t length) {
    return BuildFrame(Wire::kCtypeStatus, command, operands, length);
}

std::vector<uint8_t> OrpheusVendorCodec::BuildMasterVolumeControl(int16_t rawVolume) {
    const uint8_t ops[] = {
        static_cast<uint8_t>((static_cast<uint16_t>(rawVolume) >> 8) & 0xFF),
        static_cast<uint8_t>(static_cast<uint16_t>(rawVolume) & 0xFF)
    };
    return BuildControlFrame(OrpheusCommand::MasterVolume, ops);
}

std::vector<uint8_t> OrpheusVendorCodec::BuildMasterMuteControl(bool muted) {
    const uint8_t ops[] = {static_cast<uint8_t>(muted ? 1 : 0)};
    return BuildControlFrame(OrpheusCommand::MasterMute, ops);
}

std::vector<uint8_t> OrpheusVendorCodec::BuildVolumeSelectControl(uint16_t mask) {
    // Low byte first per hardware capture and Mario's test
    const uint8_t ops[] = {
        static_cast<uint8_t>(mask & 0xFF),
        static_cast<uint8_t>((mask >> 8) & 0xFF)
    };
    return BuildControlFrame(OrpheusCommand::VolumeSelect, ops);
}

std::optional<std::vector<uint8_t>> OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand command, uint8_t channel, uint8_t value) {
    if (channel >= Wire::kAnalogChannelCount) return std::nullopt;
    const uint8_t ops[] = {channel, value};
    return BuildControlFrame(command, ops);
}

std::optional<std::vector<uint8_t>> OrpheusVendorCodec::BuildMixInputGainControl(uint8_t bus, uint8_t input, int16_t gain) {
    if (bus >= Wire::kMixBusCount || input >= Wire::kMixInputCount) return std::nullopt;
    const uint8_t ops[] = {
        bus,
        input,
        static_cast<uint8_t>((static_cast<uint16_t>(gain) >> 8) & 0xFF),
        static_cast<uint8_t>(static_cast<uint16_t>(gain) & 0xFF)
    };
    return BuildControlFrame(OrpheusCommand::MixInputGain, ops);
}

std::optional<std::vector<uint8_t>> OrpheusVendorCodec::BuildMixBusSoloControl(uint8_t bus, uint16_t mask) {
    if (bus >= Wire::kMixBusCount) return std::nullopt;
    const uint8_t ops[] = {
        bus,
        static_cast<uint8_t>((mask >> 8) & 0xFF),
        static_cast<uint8_t>(mask & 0xFF)
    };
    return BuildControlFrame(OrpheusCommand::MixSolo, ops);
}

std::optional<std::vector<uint8_t>> OrpheusVendorCodec::BuildMixInputPositionControl(uint8_t bus, uint8_t input, int8_t position, bool isBalance) {
    if (bus >= Wire::kMixBusCount || input >= Wire::kMixInputCount) return std::nullopt;
    const uint8_t ops[] = {
        bus,
        input,
        static_cast<uint8_t>(position)
    };
    return BuildControlFrame(isBalance ? OrpheusCommand::MixInputBalance : OrpheusCommand::MixInputPan, ops);
}

std::optional<std::vector<uint8_t>> OrpheusVendorCodec::BuildMixBusGainControl(uint8_t bus, int16_t gain) {
    if (bus >= Wire::kMixBusCount) return std::nullopt;
    const uint8_t ops[] = {
        bus,
        static_cast<uint8_t>((static_cast<uint16_t>(gain) >> 8) & 0xFF),
        static_cast<uint8_t>(static_cast<uint16_t>(gain) & 0xFF)
    };
    return BuildControlFrame(OrpheusCommand::MixGain, ops);
}

std::optional<std::vector<uint8_t>> OrpheusVendorCodec::BuildMixInputMuteControl(uint8_t bus, uint8_t input, bool muted) {
    if (bus >= Wire::kMixBusCount || input >= Wire::kMixInputCount) return std::nullopt;
    const uint8_t ops[] = {bus, input, static_cast<uint8_t>(muted ? 1 : 0)};
    return BuildControlFrame(OrpheusCommand::MixInputMute, ops);
}

std::vector<uint8_t> OrpheusVendorCodec::BuildMixDefeatControl(uint8_t busMask) {
    const uint8_t ops[] = {busMask};
    return BuildControlFrame(OrpheusCommand::MixDefeat, ops);
}

std::vector<uint8_t> OrpheusVendorCodec::BuildUnitStateStatus() {
    return BuildStatusFrame(OrpheusCommand::UnitState);
}

std::optional<std::vector<uint8_t>> OrpheusVendorCodec::BuildAnalogChannelStatus(uint8_t channel) {
    if (channel >= Wire::kAnalogChannelCount) return std::nullopt;
    const uint8_t ops[] = {channel};
    return BuildStatusFrame(OrpheusCommand::AnalogChannelState, ops);
}

std::optional<std::vector<uint8_t>> OrpheusVendorCodec::BuildMixBusStatus(uint8_t bus) {
    if (bus >= Wire::kMixBusCount) return std::nullopt;
    const uint8_t ops[] = {bus};
    return BuildStatusFrame(OrpheusCommand::MixBusState, ops, Wire::kMixBusFrameLength);
}

std::vector<uint8_t> OrpheusVendorCodec::BuildMixDefeatStatus() {
    const uint8_t ops[] = {Wire::kAllMixBuses};
    return BuildStatusFrame(OrpheusCommand::MixDefeat, ops);
}

// Clock Sync (SIGNAL SOURCE)
namespace {
inline constexpr uint8_t kDestinationSubunitPlug = 0x60;
inline uint8_t DestinationPlug(bool hasAdatInput) {
    return 0x07 + (hasAdatInput ? 1 : 0);
}
inline std::optional<std::pair<uint8_t, uint8_t>> SourcePlug(OrpheusSyncSource source, bool hasAdatInput) {
    const uint8_t adat = hasAdatInput ? 1 : 0;
    switch (source) {
        case OrpheusSyncSource::Csp: return std::make_pair(0x60, static_cast<uint8_t>(0x08 + adat));
        case OrpheusSyncSource::FireWire: return std::make_pair(0xFF, 0x00);
        case OrpheusSyncSource::Local: return std::make_pair(0xFF, static_cast<uint8_t>(0x87 + adat));
        case OrpheusSyncSource::DigitalInput: return std::make_pair(0xFF, 0x85);
        case OrpheusSyncSource::Adat: return hasAdatInput ? std::make_optional(std::make_pair(0xFF, 0x86)) : std::nullopt;
        case OrpheusSyncSource::WordClock: return std::make_pair(0xFF, static_cast<uint8_t>(0x86 | adat));
    }
    return std::nullopt;
}
} // namespace

std::optional<std::vector<uint8_t>> OrpheusVendorCodec::BuildSignalSourceControl(OrpheusSyncSource source, bool hasAdatInput) {
    const auto plug = SourcePlug(source, hasAdatInput);
    if (!plug) return std::nullopt;
    return std::vector<uint8_t>{
        Wire::kCtypeControl,
        Wire::kSubunitUnit,
        Wire::kOpcodeSignalSource,
        0x0F,
        plug->first,
        plug->second,
        kDestinationSubunitPlug,
        DestinationPlug(hasAdatInput)
    };
}

std::vector<uint8_t> OrpheusVendorCodec::BuildSignalSourceStatus(bool hasAdatInput) {
    return std::vector<uint8_t>{
        Wire::kCtypeStatus,
        Wire::kSubunitUnit,
        Wire::kOpcodeSignalSource,
        0x0F,
        0xFF,
        0xFF,
        kDestinationSubunitPlug,
        DestinationPlug(hasAdatInput)
    };
}

std::optional<OrpheusSyncSource> OrpheusVendorCodec::ParseSignalSourceStatus(std::span<const uint8_t> response, bool hasAdatInput) {
    if (response.size() < 6 || response[0] != Wire::kResponseStable) return std::nullopt;
    if (response[4] == 0x60) return OrpheusSyncSource::Csp;
    if (response[4] != 0xFF) return std::nullopt;

    const uint8_t plug = response[5];
    if (plug == 0x00) return OrpheusSyncSource::FireWire;
    if (plug == 0x85) return OrpheusSyncSource::DigitalInput;
    if (!hasAdatInput) {
        if (plug == 0x86) return OrpheusSyncSource::WordClock;
        if (plug == 0x87) return OrpheusSyncSource::Local;
    } else {
        if (plug == 0x86) return OrpheusSyncSource::Adat;
        if (plug == 0x87) return OrpheusSyncSource::WordClock;
        if (plug == 0x88) return OrpheusSyncSource::Local;
    }
    return std::nullopt;
}

std::optional<std::span<const uint8_t>> OrpheusVendorCodec::ValidateResponse(std::span<const uint8_t> response,
                                                                             OrpheusCommand command,
                                                                             bool isControl) {
    if (response.size() < Wire::kPayloadOffset) return std::nullopt;
    const uint8_t expectedCode = isControl ? Wire::kResponseAccepted : Wire::kResponseStable;
    if (response[0] != expectedCode) return std::nullopt;
    if (response[1] != Wire::kSubunitAudio) return std::nullopt;
    if (response[3] != Wire::kCompanyId[0] ||
        response[4] != Wire::kCompanyId[1] ||
        response[5] != Wire::kCompanyId[2]) {
        return std::nullopt;
    }
    if (response[Wire::kCommandOffset] != static_cast<uint8_t>(command)) {
        return std::nullopt;
    }
    return response.subspan(Wire::kPayloadOffset);
}

std::optional<OrpheusFirmwareVersion> OrpheusVendorCodec::ParseFirmwareVersion(std::span<const uint8_t> response) {
    const auto payload = ValidateResponse(response, OrpheusCommand::FirmwareVersion, false);
    if (!payload || payload->size() < 2) return std::nullopt;
    return OrpheusFirmwareVersion{
        .major = (*payload)[0],
        .minor = (*payload)[1]
    };
}

std::optional<OrpheusUnitState> OrpheusVendorCodec::ParseUnitState(std::span<const uint8_t> response) {
    const auto payload = ValidateResponse(response, OrpheusCommand::UnitState, false);
    if (!payload || payload->size() < 8) return std::nullopt;
    const auto p = *payload;
    const int16_t volume = static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    const uint16_t mask = static_cast<uint16_t>((static_cast<uint16_t>(p[2]) << 8) | p[3]);
    const uint8_t flags = p[4];
    return OrpheusUnitState{
        .masterVolume = volume,
        .volumeSelectMask = mask,
        .masterMute = (flags & 0x01) != 0,
        .masterLock = (flags & 0x02) != 0,
        .identify = (flags & 0x04) != 0,
        .frontPanelMeterMode = static_cast<uint8_t>(flags >> 4),
        .sampleRateConverter = static_cast<uint8_t>(p[5] & 0x0F),
        .clockOutput = static_cast<uint8_t>(p[5] >> 4),
        .adatMode = p[6],
        .headphoneSource = static_cast<uint8_t>(p[7] & 0x0F),
        .meterBrightness = static_cast<uint8_t>(p[7] >> 4)
    };
}

std::optional<OrpheusAnalogChannelState> OrpheusVendorCodec::ParseAnalogChannelState(std::span<const uint8_t> response, uint32_t channel) {
    const auto payload = ValidateResponse(response, OrpheusCommand::AnalogChannelState, false);
    if (!payload || payload->size() < 3) return std::nullopt;
    const auto p = *payload;
    // Format: bytes[0] is channel, bytes[1] is inputType, bytes[2] is flags
    const uint8_t inputType = p[1];
    const uint8_t flags = p[2];
    OrpheusAnalogChannelState state{
        .inputType = inputType,
        .lineInputLevel = static_cast<uint8_t>(flags & 0x01),
        .lineOutputLevel = static_cast<uint8_t>((flags >> 1) & 0x01),
        .overkiller = (flags & 0x04) != 0,
        .midSide = (flags & 0x08) != 0,
        .phantom = (flags & 0x20) != 0,
        .phase = (flags & 0x40) != 0,
        .filter = 0,
        .micGain = 10,
        .instrumentGain = 10
    };
    if (channel < 4 && payload->size() >= 5) {
        state.filter = p[3];
        state.micGain = p[4];
    }
    if (channel < 2 && payload->size() >= 6) {
        state.instrumentGain = p[5];
    }
    return state;
}

std::optional<OrpheusMixBusState> OrpheusVendorCodec::ParseMixBusState(std::span<const uint8_t> response, uint32_t bus) {
    const auto payload = ValidateResponse(response, OrpheusCommand::MixBusState, false);
    if (!payload || payload->size() < 67) return std::nullopt;
    const auto p = *payload;
    if (p[0] != bus) return std::nullopt;
    const int16_t gain = static_cast<int16_t>((static_cast<uint16_t>(p[1]) << 8) | p[2]);
    const bool mute = (p[3] != 0);
    const uint16_t soloMask = static_cast<uint16_t>((static_cast<uint16_t>(p[4]) << 8) | p[5]);
    
    OrpheusMixBusState state{
        .busIndex = static_cast<uint8_t>(bus),
        .masterGain = gain,
        .masterMute = mute,
        .soloMask = soloMask
    };

    for (size_t i = 0; i < Wire::kMixInputCount; ++i) {
        const size_t base = 7 + i * 5;
        if (base + 4 >= p.size()) break;
        state.inputGains[i] = static_cast<int16_t>((static_cast<uint16_t>(p[base]) << 8) | p[base + 1]);
        state.inputMutes[i] = (p[base + 2] != 0);
        state.inputPans[i] = static_cast<int8_t>(p[base + 4]);
    }
    return state;
}

} // namespace ASFW::Audio::Prism::Orpheus
