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
        if (input.activeRateHz.has_value() && output.activeRateHz.has_value() &&
            input.activeRateHz != output.activeRateHz) {
            return std::nullopt;
        }
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
                const bool outputSupports = std::any_of(
                    output.supportedFormations.begin(), output.supportedFormations.end(),
                    [hz](const StreamFormation& candidate) { return candidate.RateHz() == hz; });
                if (outputSupports) addRate(*hz);
            }
        }
        return rates;
    }

    [[nodiscard]] std::optional<StreamFormation> InputFormationAtRate(uint32_t rateHz) const noexcept {
        return FormationAtRate(input, rateHz);
    }

    [[nodiscard]] std::optional<StreamFormation> OutputFormationAtRate(uint32_t rateHz) const noexcept {
        return FormationAtRate(output, rateHz);
    }

    /// Return the currently reported rate only when both directions have a
    /// formation for it. If no current rate is known, select the first duplex
    /// rate advertised by both directions. A known but unsupported current rate
    /// leaves geometry unavailable instead of silently substituting another rate.
    [[nodiscard]] std::optional<uint32_t> SelectDuplexRateHz() const noexcept {
        // Conflicting reports mean the device has no trustworthy duplex clock.
        // Do not turn that disagreement into an apparently valid advertised rate.
        if ((input.activeRateHz.has_value() && output.activeRateHz.has_value() &&
             input.activeRateHz != output.activeRateHz) ||
            HasConflictingCurrentRateCodes()) {
            return std::nullopt;
        }
        if (const auto current = CurrentRateHz()) {
            return InputFormationAtRate(*current).has_value() &&
                           OutputFormationAtRate(*current).has_value()
                       ? current
                       : std::nullopt;
        }
        const auto rates = SupportedRatesHz();
        return rates.empty() ? std::nullopt : std::optional<uint32_t>{rates.front()};
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

        for (const auto& in : input.supportedFormations) {
            if (in.pcmChannels != pcmChannels || in.midiSlots != midiSlots) continue;
            for (const auto& out : output.supportedFormations) {
                if (out.rateCode == in.rateCode && out.pcmChannels == pcmChannels &&
                    out.midiSlots == midiSlots) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    [[nodiscard]] bool HasConflictingCurrentRateCodes() const noexcept {
        const auto inputRate = input.currentFormat.has_value() ? input.currentFormat->formation : std::nullopt;
        const auto outputRate = output.currentFormat.has_value() ? output.currentFormat->formation : std::nullopt;
        return inputRate.has_value() && outputRate.has_value() &&
               inputRate->rateCode != outputRate->rateCode;
    }

    [[nodiscard]] static std::optional<StreamFormation> FormationAtRate(
        const IsochronousPlugModel& plug, uint32_t rateHz) noexcept {
        const auto found = std::find_if(
            plug.supportedFormations.begin(), plug.supportedFormations.end(),
            [rateHz](const StreamFormation& formation) { return formation.RateHz() == rateHz; });
        return found == plug.supportedFormations.end() ? std::nullopt
                                                       : std::optional<StreamFormation>{*found};
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
