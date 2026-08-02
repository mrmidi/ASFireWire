// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBPlug0StreamDiscovery.hpp — Bounded, observational BeBoB discovery.
//
// STATUS-only inventory of a BeBoB device's isochronous plug 0 pair: unit plug
// counts, ISO plug type, stream formations, channel positions, and section types.
// Never changes device clock, rate, routing, CMP, PCR, or stream state.
//
// Scope: plug-0 + one-CMP-connection. Name is deliberate — this does NOT iterate
// all advertised plugs. Generalize to BeBoBStreamDiscovery when multi-plug BeBoB
// devices need discovery.
//
// Wire behavior cross-validated with Linux sound/firewire/bebob/
// bebob_command.c:91-107, 289-328 and bebob_stream.c:254-370, 705-820.
// Fresh implementation; no reference source is copied.

#pragma once

#include "../../../Protocols/AVC/IAVCCommandSubmitter.hpp"
#include "../../../DeviceProfiles/Audio/Vendors/BeBoBDeviceProfiles.hpp"

using ::ASFW::Protocols::AVC::IAVCCommandSubmitter;
using ::ASFW::Protocols::AVC::AVCCdb;
using ::ASFW::Protocols::AVC::AVCResult;
using ::ASFW::Protocols::AVC::AVCCompletion;
using ::ASFW::Protocols::AVC::kAVCSubunitUnit;
using ::ASFW::Protocols::AVC::AVCCommandType;

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ASFW::Audio::BeBoB {

struct StreamFormation {
    uint8_t rateCode{0};
    uint8_t pcmChannels{0};
    uint8_t midiSlots{0};
};

enum class PlugDirection : uint8_t { kInput = 0x00, kOutput = 0x01 };
enum class StreamFormatState : uint8_t { kActive = 0x00, kInactive = 0x01, kNoStreamFormat = 0x02 };

// Linux BeBoB discovery starts with generic unit PLUG_INFO, then asks the
// BridgeCo extension about ISO plug 0 in each direction. Keep these commands
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
    // BridgeCo section type, e.g. 0x0a for MIDI. Remains unknown until the
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

    /// True when both host-to-device and device-to-host ISO plug 0 advertise
    /// the same AM824 slot geometry. Rate selection remains a separate
    /// operation: a formation list is capability data, not the current clock.
    [[nodiscard]] bool SupportsDuplexFormation(uint8_t pcmChannels,
                                                uint8_t midiSlots) const noexcept;
};

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

/// Sends STATUS queries only. Never changes device clock, rate, routing, CMP,
/// PCR, or stream state. Scopes to the ISO plug-0 pair.
void StartBeBoBPlug0Discovery(IAVCCommandSubmitter& submitter, uint64_t guid,
                              ReadOnlyProbeCompletion completion = {});

} // namespace ASFW::Audio::BeBoB
