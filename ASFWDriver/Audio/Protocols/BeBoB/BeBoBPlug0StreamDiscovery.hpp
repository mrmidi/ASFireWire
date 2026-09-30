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

#include "../../../Protocols/AVC/Core/IAvcUnit.hpp"
#include "../../../Protocols/AVC/Core/RateCodes.hpp"
#include "../../../Protocols/AVC/Commands/GeneralCommands.hpp"

#include <algorithm>
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

    [[nodiscard]] constexpr std::optional<uint32_t> RateHz() const noexcept {
        return AVC::ToHz(static_cast<AVC::StreamFormatRate>(rateCode));
    }
};

enum class PlugDirection : uint8_t { kInput = 0x00, kOutput = 0x01 };
enum class StreamFormatState : uint8_t { kActive = 0x00, kInactive = 0x01, kNoStreamFormat = 0x02 };

// Linux BeBoB discovery starts with generic unit PLUG_INFO, then asks the
// BridgeCo extension about ISO plug 0 in each direction, followed by signal format.
enum class ReadOnlyProbeCommand : uint8_t {
    kUnitPlugCounts,
    kIsochPlugType,
    kStreamFormatList,
    kChannelPositions,
    kSectionType,
    kSignalFormat,
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
    std::optional<uint32_t> activeRateHz{};
};

using UnitPlugCounts = ASFW::AVC::Cmd::UnitPlugCounts;

struct DeviceModel {
    std::optional<UnitPlugCounts> unitPlugCounts{};
    IsochronousPlugModel input{};   // Host -> device playback.
    IsochronousPlugModel output{};  // Device -> host capture.
    std::optional<uint32_t> currentRateHz{};

    [[nodiscard]] bool HasAgreedCurrentRate() const noexcept {
        return CurrentRateCode().has_value() ||
               (input.activeRateHz.has_value() && output.activeRateHz.has_value() &&
                input.activeRateHz == output.activeRateHz);
    }

    [[nodiscard]] std::optional<uint8_t> CurrentRateCode() const noexcept {
        const auto inputRate = input.currentFormat.has_value() ? input.currentFormat->formation : std::nullopt;
        const auto outputRate = output.currentFormat.has_value() ? output.currentFormat->formation : std::nullopt;
        if (!inputRate.has_value() || !outputRate.has_value() || inputRate->rateCode != outputRate->rateCode) {
            return std::nullopt;
        }
        return inputRate->rateCode;
    }

    [[nodiscard]] std::optional<uint32_t> CurrentRateHz() const noexcept {
        if (currentRateHz.has_value()) {
            return currentRateHz;
        }
        if (input.activeRateHz.has_value() && output.activeRateHz.has_value() &&
            input.activeRateHz == output.activeRateHz) {
            return input.activeRateHz;
        }
        if (input.activeRateHz.has_value() && !output.activeRateHz.has_value()) {
            return input.activeRateHz;
        }
        if (output.activeRateHz.has_value() && !input.activeRateHz.has_value()) {
            return output.activeRateHz;
        }
        if (const auto code = CurrentRateCode()) {
            return AVC::ToHz(static_cast<AVC::StreamFormatRate>(*code));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<uint32_t> SupportedRatesHz() const noexcept {
        std::vector<uint32_t> rates;
        const auto addRate = [&rates](uint32_t hz) {
            if (hz > 0 && std::find(rates.begin(), rates.end(), hz) == rates.end()) {
                rates.push_back(hz);
            }
        };
        for (const auto& formation : input.supportedFormations) {
            if (const auto hz = formation.RateHz()) {
                addRate(*hz);
            }
        }
        for (const auto& formation : output.supportedFormations) {
            if (const auto hz = formation.RateHz()) {
                addRate(*hz);
            }
        }
        return rates;
    }

    /// True when both host-to-device and device-to-host ISO plug 0 advertise
    /// the same AM824 slot geometry. Rate selection remains a separate
    /// operation: a formation list is capability data, not the current clock.
    [[nodiscard]] bool SupportsDuplexFormation(uint8_t pcmChannels,
                                               uint8_t midiSlots) const noexcept {
        if (!unitPlugCounts.has_value() ||
            unitPlugCounts->isochronousInputs == 0 ||
            unitPlugCounts->isochronousOutputs == 0) {
            return false;
        }

        const auto supports = [pcmChannels, midiSlots](const IsochronousPlugModel& plug) {
            for (const auto& formation : plug.supportedFormations) {
                if (formation.pcmChannels == pcmChannels && formation.midiSlots == midiSlots) {
                    return true;
                }
            }
            return false;
        };
        return supports(input) && supports(output);
    }
};

[[nodiscard]] std::optional<StreamFormation>
ParseExtendedStreamFormatListResponse(uint8_t requestedIndex,
                                      std::span<const uint8_t> operands) noexcept;

[[nodiscard]] std::optional<CurrentStreamFormat>
ParseExtendedStreamFormatSingleResponse(std::span<const uint8_t> operands) noexcept;

[[nodiscard]] std::optional<std::vector<ChannelSection>>
ParseChannelPositionSections(std::span<const uint8_t> payload) noexcept;

using ReadOnlyProbeCompletion = std::function<void(const DeviceModel&)>;

[[nodiscard]] std::optional<StreamFormation>
ParseStreamFormation(std::span<const uint8_t> formation) noexcept;

/// Sends STATUS queries only. Never changes device clock, rate, routing, CMP,
/// PCR, or stream state. Scopes to the ISO plug-0 pair.
void StartBeBoBPlug0Discovery(ASFW::AVC::IAvcUnit& unit, uint64_t guid,
                              ReadOnlyProbeCompletion completion = {});

} // namespace ASFW::Audio::BeBoB
