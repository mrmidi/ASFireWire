// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BridgeCoInventory.cpp - see BridgeCoInventory.hpp.

#include "BridgeCoInventory.hpp"

#include "../../../Logging/Logging.hpp"
#include "../../../Protocols/AVC/Commands/GeneralCommands.hpp"
#include "../../../Protocols/AVC/Extensions/BridgeCoPlugInfo.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace ASFW::Audio::BeBoB {
namespace {
namespace A = ASFW::AVC;
namespace E = ASFW::AVC::DiscoveryEngine;

/// One probe run: positions then section types, playback then capture.
class SectionProbe final : public std::enable_shared_from_this<SectionProbe> {
public:
    SectionProbe(A::IAvcUnit& unit, uint64_t guid, std::function<void(ChannelSections)> completion)
        : unit_(unit), guid_(guid), completion_(std::move(completion)) {}

    void Start() { AskPositions(A::Cmd::PlugDirection::kInput); }

private:
    [[nodiscard]] std::vector<ChannelSection>& Sections(A::Cmd::PlugDirection direction) {
        return direction == A::Cmd::PlugDirection::kInput ? result_.playback : result_.capture;
    }
    static const char* Name(A::Cmd::PlugDirection direction) {
        return direction == A::Cmd::PlugDirection::kInput ? "input" : "output";
    }

    void AskPositions(A::Cmd::PlugDirection direction) {
        auto* unit = unit_.Get();
        if (!unit) return; // Unit gone: its discovery session went with it.
        unit->Status(
            A::BridgeCo::ExtendedPlugInfoCommand{.operands = {
                .plug = A::Cmd::PlugAddress::UnitPlug(direction, A::Cmd::UnitPlugType::kPcr, 0),
                .type = A::BridgeCo::InfoType::kChannelPositions}},
            [self = shared_from_this(), direction](A::Expected<A::BridgeCo::ExtendedPlugInfoReply> reply) {
                if (!self->unit_) return;
                if (!reply) {
                    ASFW_LOG(AVC, "BridgeCo: ISO %{public}s channel positions unavailable result=%u GUID=0x%016llx",
                             Name(direction), static_cast<unsigned>(reply.error().kind), self->guid_);
                } else if (auto sections = ParseChannelPositionSections(reply->Data())) {
                    ASFW_LOG(AVC, "BridgeCo: ISO %{public}s channel map sections=%zu GUID=0x%016llx",
                             Name(direction), sections->size(), self->guid_);
                    self->Sections(direction) = std::move(*sections);
                } else {
                    ASFW_LOG(AVC, "BridgeCo: ISO %{public}s channel map malformed GUID=0x%016llx",
                             Name(direction), self->guid_);
                }
                self->AskSectionType(direction, 0);
            });
    }

    void AskSectionType(A::Cmd::PlugDirection direction, uint8_t section) {
        if (section >= Sections(direction).size()) {
            if (direction == A::Cmd::PlugDirection::kInput) { AskPositions(A::Cmd::PlugDirection::kOutput); return; }
            if (!unit_) return;
            if (completion_) std::exchange(completion_, {})(std::move(result_));
            return;
        }
        auto* unit = unit_.Get();
        if (!unit) return;
        unit->Status(
            A::BridgeCo::ExtendedPlugInfoCommand{.operands = {
                .plug = A::Cmd::PlugAddress::UnitPlug(direction, A::Cmd::UnitPlugType::kPcr, 0),
                .type = A::BridgeCo::InfoType::kClusterInfo,
                .extra = static_cast<uint8_t>(section + 1U)}},
            [self = shared_from_this(), direction, section](A::Expected<A::BridgeCo::ExtendedPlugInfoReply> reply) {
                if (!self->unit_) return;
                // The reply echoes the 1-based section id, then the section
                // type (Linux bebob_command.c:228, :246).
                if (reply) {
                    if (auto type = reply->AsClusterPortType(static_cast<uint8_t>(section + 1U))) {
                        self->Sections(direction)[section].type = static_cast<uint8_t>(*type);
                    }
                }
                self->AskSectionType(direction, static_cast<uint8_t>(section + 1U));
            });
    }

    Common::LiveRef<A::IAvcUnit> unit_;
    uint64_t guid_;
    std::function<void(ChannelSections)> completion_;
    ChannelSections result_;
};

/// The unit ISO plug 0 facts discovery committed for one direction.
const E::PlugContents* IsoPlug0(const E::DiscoverySnapshot& snapshot, A::Cmd::PlugDirection direction) {
    for (const auto& plug : snapshot.plugs)
        if (plug.address.IsUnit() && plug.direction == direction && plug.id.value == 0) return &plug;
    return nullptr;
}

std::optional<uint32_t> SignalRateHz(const E::PlugContents* plug) {
    if (!plug || !plug->signalFormat) return std::nullopt;
    const auto sfc = A::Cmd::SfcOf(*plug->signalFormat);
    return sfc ? A::ToHz(*sfc) : std::nullopt;
}

void Formations(const E::PlugContents* plug, E::ExtensionPlug& out) {
    if (!plug) return;
    for (const auto& format : plug->formations) {
        if (format.kind != A::Cmd::StreamFormat::Kind::kCompoundAm824) continue;
        const auto hz = A::ToHz(format.compound.rate);
        if (!hz) continue;
        if (!out.formations.push_back({.rateHz = *hz, .pcmChannels = format.compound.PcmChannels(),
                                       .midiChannels = format.compound.MidiChannels()})) break; // Full.
    }
}
} // namespace

std::optional<std::vector<ChannelSection>> ParseChannelPositionSections(std::span<const uint8_t> payload) noexcept {
    if (payload.empty() || payload[0] > kMaxChannelSections) return std::nullopt;
    std::vector<ChannelSection> result;
    result.reserve(payload[0]);
    size_t cursor = 1;
    for (uint8_t section = 0; section < payload[0]; ++section) {
        if (cursor >= payload.size()) return std::nullopt;
        const uint8_t channelCount = payload[cursor++];
        if (channelCount > (payload.size() - cursor) / 2U) return std::nullopt;
        ChannelSection parsed{};
        parsed.positions.reserve(channelCount);
        for (uint8_t channel = 0; channel < channelCount; ++channel) {
            const uint8_t streamPosition = payload[cursor++];
            const uint8_t sectionLocation = payload[cursor++];
            if (streamPosition == 0 || sectionLocation == 0) return std::nullopt;
            parsed.positions.push_back(ChannelPosition{
                .streamPosition = static_cast<uint8_t>(streamPosition - 1U),
                .sectionLocation = static_cast<uint8_t>(sectionLocation - 1U),
            });
        }
        result.push_back(std::move(parsed));
    }
    return cursor == payload.size() ? std::optional{std::move(result)} : std::nullopt;
}

void ProbeChannelSections(A::IAvcUnit& unit, uint64_t guid, std::function<void(ChannelSections)> completion) {
    std::make_shared<SectionProbe>(unit, guid, std::move(completion))->Start();
}

bool HasDuplexIsoPlugPair(const E::DiscoverySnapshot& snapshot) {
    return snapshot.unit.unitPlugs.isochronousInputs > 0 && snapshot.unit.unitPlugs.isochronousOutputs > 0;
}

E::ExtensionFacts BridgeCoFormationFacts(const E::DiscoverySnapshot& snapshot) {
    E::ExtensionFacts facts;
    const auto* input = IsoPlug0(snapshot, A::Cmd::PlugDirection::kInput);   // Host playback.
    const auto* output = IsoPlug0(snapshot, A::Cmd::PlugDirection::kOutput); // Host capture.
    Formations(input, facts.playback);
    Formations(output, facts.capture);
    // The current rate is what both signal formats report (Linux
    // bebob_stream.c:66-81 reads it this way); disagreement means unknown.
    const auto in = SignalRateHz(input), out = SignalRateHz(output);
    const uint32_t rate = in && out && *in == *out ? *in : 0;
    facts.playback.currentRateHz = rate;
    facts.capture.currentRateHz = rate;
    return facts;
}

} // namespace ASFW::Audio::BeBoB
