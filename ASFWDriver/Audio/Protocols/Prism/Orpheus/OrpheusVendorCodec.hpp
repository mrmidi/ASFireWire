// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "OrpheusTypes.hpp"
#include <optional>
#include <span>
#include <vector>

namespace ASFW::Audio::Prism::Orpheus {

class OrpheusVendorCodec final {
public:
    // Generic frame builders
    static std::vector<uint8_t> BuildFrame(uint8_t ctype,
                                           OrpheusCommand command,
                                           std::span<const uint8_t> operands = {},
                                           size_t length = Wire::kFrameLength);

    static std::vector<uint8_t> BuildControlFrame(OrpheusCommand command,
                                                  std::span<const uint8_t> operands,
                                                  size_t length = Wire::kFrameLength);

    static std::vector<uint8_t> BuildStatusFrame(OrpheusCommand command,
                                                 std::span<const uint8_t> operands = {},
                                                 size_t length = Wire::kFrameLength);

    // Specialized Control Builders
    static std::vector<uint8_t> BuildMasterVolumeControl(int16_t rawVolume);
    static std::vector<uint8_t> BuildMasterMuteControl(bool muted);
    static std::vector<uint8_t> BuildVolumeSelectControl(uint16_t mask);
    static std::optional<std::vector<uint8_t>> BuildAnalogControl(OrpheusCommand command, uint8_t channel, uint8_t value);
    static std::optional<std::vector<uint8_t>> BuildMixInputGainControl(uint8_t bus, uint8_t input, int16_t gain);
    static std::optional<std::vector<uint8_t>> BuildMixBusSoloControl(uint8_t bus, uint16_t mask);
    static std::optional<std::vector<uint8_t>> BuildMixInputPositionControl(uint8_t bus, uint8_t input, int8_t position, bool isBalance);
    static std::optional<std::vector<uint8_t>> BuildMixBusGainControl(uint8_t bus, int16_t gain);
    static std::optional<std::vector<uint8_t>> BuildMixInputMuteControl(uint8_t bus, uint8_t input, bool muted);
    static std::vector<uint8_t> BuildMixDefeatControl(uint8_t busMask);

    // Specialized Status Builders
    static std::vector<uint8_t> BuildUnitStateStatus();
    static std::optional<std::vector<uint8_t>> BuildAnalogChannelStatus(uint8_t channel);
    static std::optional<std::vector<uint8_t>> BuildMixBusStatus(uint8_t bus);
    static std::vector<uint8_t> BuildMixDefeatStatus();

    // Signal Source (Clock Sync)
    static std::optional<std::vector<uint8_t>> BuildSignalSourceControl(OrpheusSyncSource source, bool hasAdatInput);
    static std::vector<uint8_t> BuildSignalSourceStatus(bool hasAdatInput);
    static std::optional<OrpheusSyncSource> ParseSignalSourceStatus(std::span<const uint8_t> response, bool hasAdatInput);

    // Response Validation & Parsing
    static std::optional<std::span<const uint8_t>> ValidateResponse(std::span<const uint8_t> response,
                                                                    OrpheusCommand command,
                                                                    bool isControl);

    static std::optional<OrpheusFirmwareVersion> ParseFirmwareVersion(std::span<const uint8_t> response);
    static std::optional<OrpheusUnitState> ParseUnitState(std::span<const uint8_t> response);
    static std::optional<OrpheusAnalogChannelState> ParseAnalogChannelState(std::span<const uint8_t> response, uint32_t channel);
    static std::optional<OrpheusMixBusState> ParseMixBusState(std::span<const uint8_t> response, uint32_t bus);
};

} // namespace ASFW::Audio::Prism::Orpheus
