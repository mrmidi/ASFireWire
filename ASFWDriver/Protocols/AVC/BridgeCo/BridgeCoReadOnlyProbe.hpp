// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Bounded, observational BeBoB discovery for exact known devices.

#pragma once

#include "../IAVCCommandSubmitter.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ASFW::Protocols::AVC::BridgeCo {

inline constexpr uint32_t kTerraTecVendorId = 0x000aac;
inline constexpr uint32_t kPhase88RackFwModelId = 0x000003;

[[nodiscard]] constexpr bool IsTerraTecPhase88RackFw(uint32_t vendorId,
                                                       uint32_t modelId) noexcept {
    return vendorId == kTerraTecVendorId && modelId == kPhase88RackFwModelId;
}

struct StreamFormation {
    uint8_t rateCode{0};
    uint8_t pcmChannels{0};
    uint8_t midiSlots{0};
};

enum class PlugDirection : uint8_t { kInput = 0x00, kOutput = 0x01 };
enum class StreamFormatState : uint8_t { kActive = 0x00, kInactive = 0x01, kNoStreamFormat = 0x02 };

// Linux BeBoB discovery starts with generic unit PLUG_INFO, then asks the
// BridgeCo extension about ISO plug 0 in each direction.  Keep these commands
// explicit and testable so their CDB operand offsets cannot drift.
enum class ReadOnlyProbeCommand : uint8_t {
    kUnitPlugCounts,
    kIsochPlugType,
    kStreamFormatList,
    kChannelPositions,
    kSectionType,
};

struct ChannelPosition {
    // Both fields are zero-based in ASFW. BridgeCo encodes each as one-based.
    uint8_t streamPosition{0};
    uint8_t sectionLocation{0};
};

struct ChannelSection {
    // BridgeCo section type, e.g. 0x0a for MIDI. It remains unknown until the
    // independent section-info request succeeds.
    std::optional<uint8_t> type{};
    std::vector<ChannelPosition> positions{};
};

struct CurrentStreamFormat {
    StreamFormatState state{StreamFormatState::kNoStreamFormat};
    std::optional<StreamFormation> formation{};
};

struct IsochronousPlugModel {
    std::optional<uint8_t> plugType{};
    std::optional<uint8_t> channelCount{};
    std::vector<StreamFormation> supportedFormations{};
    std::optional<CurrentStreamFormat> currentFormat{};
    std::vector<ChannelSection> channelSections{};
};

struct UnitPlugCounts {
    uint8_t isochronousInputs{0};
    uint8_t isochronousOutputs{0};
    uint8_t externalInputs{0};
    uint8_t externalOutputs{0};
};

struct DeviceModel {
    std::optional<UnitPlugCounts> unitPlugCounts{};
    IsochronousPlugModel input{};   // Host -> device playback.
    IsochronousPlugModel output{};  // Device -> host capture.

    [[nodiscard]] bool HasAgreedCurrentRate() const noexcept;
    [[nodiscard]] std::optional<uint8_t> CurrentRateCode() const noexcept;
};

// A BridgeCo extended stream-format-list response echoes the requested list
// index in operand 7; the AM824 formation begins at operand 8.  Keeping this
// codec public makes the discovery trace and the BeBoB backend share exactly
// one wire interpretation.
[[nodiscard]] std::optional<StreamFormation>
ParseExtendedStreamFormatListResponse(uint8_t requestedIndex,
                                      std::span<const uint8_t> operands) noexcept;

[[nodiscard]] std::optional<CurrentStreamFormat>
ParseExtendedStreamFormatSingleResponse(std::span<const uint8_t> operands) noexcept;

[[nodiscard]] std::optional<std::vector<ChannelSection>>
ParseChannelPositionSections(std::span<const uint8_t> payload) noexcept;

[[nodiscard]] AVCCdb BuildReadOnlyProbeCommand(ReadOnlyProbeCommand command,
                                                PlugDirection direction = PlugDirection::kInput,
                                                uint8_t index = 0) noexcept;

using ReadOnlyProbeCompletion = std::function<void(const DeviceModel&)>;

[[nodiscard]] std::optional<StreamFormation>
ParseStreamFormation(std::span<const uint8_t> formation) noexcept;

/// Sends STATUS queries only. It never changes device clock, rate, routing,
/// CMP, PCR, or stream state.
void StartPhase88ReadOnlyProbe(IAVCCommandSubmitter& submitter, uint64_t guid,
                               ReadOnlyProbeCompletion completion = {});

} // namespace ASFW::Protocols::AVC::BridgeCo
