// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBPlug0StreamDiscovery.cpp — Bounded, observational BeBoB discovery.
//
// STATUS-only inventory of a BeBoB device's isochronous plug 0 pair.
// Wire behavior cross-validated with Linux sound/firewire/bebob/
// bebob_command.c:91-107, 289-328 and bebob_stream.c:254-370, 705-820.
// Fresh implementation; no reference source is copied.

#include "BeBoBPlug0StreamDiscovery.hpp"

#include "../../../Logging/Logging.hpp"
#include "../../../Protocols/AVC/Commands/GeneralCommands.hpp"
#include "../../../Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "../../../Protocols/AVC/Extensions/BridgeCoPlugInfo.hpp"

#include <algorithm>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::Audio::BeBoB {
namespace {

constexpr uint8_t kExtendedPlugInfo = 0xc0;
constexpr uint8_t kMaxFormatEntries = 8;
constexpr uint8_t kMaxSections = 16;

struct Request { ReadOnlyProbeCommand command; PlugDirection direction; uint8_t index{0}; };

[[nodiscard]] const char* DirectionName(PlugDirection direction) noexcept {
    return direction == PlugDirection::kInput ? "input" : "output";
}

[[nodiscard]] const char* RequestName(ReadOnlyProbeCommand command) noexcept {
    switch (command) {
        case ReadOnlyProbeCommand::kUnitPlugCounts: return "unit plug counts";
        case ReadOnlyProbeCommand::kIsochPlugType: return "ISO plug type";
        case ReadOnlyProbeCommand::kStreamFormatList: return "stream format";
        case ReadOnlyProbeCommand::kChannelPositions: return "channel map";
        case ReadOnlyProbeCommand::kSectionType: return "section type";
    }
    return "unknown";
}

class Probe final : public std::enable_shared_from_this<Probe> {
public:
    Probe(ASFW::AVC::IAvcUnit& unit, uint64_t guid, ReadOnlyProbeCompletion completion)
        : unit_(unit), guid_(guid), completion_(std::move(completion)) {
        // Linux BeBoB begins with generic unit PLUG_INFO, without UNIT_INFO or
        // SUBUNIT_INFO. Cross-validated: bebob_stream.c:908-940.
        queue_.push_back({ReadOnlyProbeCommand::kUnitPlugCounts, PlugDirection::kInput});
    }

    void Start() {
        ASFW_LOG(AVC,
                 "BeBoBProbe: BeBoB device matched; starting STATUS-only BridgeCo inventory GUID=0x%016llx",
                 guid_);
        SubmitNext();
    }

private:
    void SubmitNext() {
        if (next_ == queue_.size()) {
            ASFW_LOG(AVC, "BeBoBProbe: inventory complete GUID=0x%016llx", guid_);
            if (completion_) completion_(model_);
            return;
        }
        const Request request = queue_[next_++];
        auto self = shared_from_this();

        switch (request.command) {
            case ReadOnlyProbeCommand::kUnitPlugCounts: {
                unit_.Status(
                    ASFW::AVC::Cmd::PlugInfoCommand{
                        .operands = ASFW::AVC::Cmd::PlugInfoOperands{
                            .form = ASFW::AVC::Cmd::PlugInfoForm::kUnitIsoExternal,
                            .dummyByte = 0x00,
                        },
                    },
                    [self, request](ASFW::AVC::Expected<ASFW::AVC::Cmd::PlugInfoReply> res) {
                        if (!res) {
                            self->HandleError(request, res.error());
                        } else {
                            self->HandleUnitPlugCounts(*res);
                        }
                        self->SubmitNext();
                    });
                break;
            }
            case ReadOnlyProbeCommand::kIsochPlugType: {
                const auto dir = request.direction == PlugDirection::kInput ?
                    ASFW::AVC::Cmd::PlugDirection::kInput : ASFW::AVC::Cmd::PlugDirection::kOutput;
                unit_.Status(
                    ASFW::AVC::BridgeCo::ExtendedPlugInfoCommand{
                        .operands = ASFW::AVC::BridgeCo::ExtendedPlugInfoOperands{
                            .plug = ASFW::AVC::Cmd::PlugAddress::UnitPlug(dir, ASFW::AVC::Cmd::UnitPlugType::kPcr, 0),
                            .type = ASFW::AVC::BridgeCo::InfoType::kPlugType,
                        },
                    },
                    [self, request](ASFW::AVC::Expected<ASFW::AVC::BridgeCo::ExtendedPlugInfoReply> res) {
                        if (!res) {
                            self->HandleError(request, res.error());
                        } else {
                            auto plugType = res->AsPlugType();
                            if (plugType) {
                                const uint8_t value = static_cast<uint8_t>(*plugType);
                                self->Plug(request.direction).plugType = value;
                                ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s plug 0 type=0x%02x GUID=0x%016llx",
                                         DirectionName(request.direction), value, self->guid_);
                            }
                        }
                        self->SubmitNext();
                    });
                break;
            }
            case ReadOnlyProbeCommand::kStreamFormatList: {
                const auto dir = request.direction == PlugDirection::kInput ?
                    ASFW::AVC::Cmd::PlugDirection::kInput : ASFW::AVC::Cmd::PlugDirection::kOutput;
                unit_.Status(
                    ASFW::AVC::Cmd::StreamFormatCommand{
                        .operands = ASFW::AVC::Cmd::StreamFormatOperands{
                            .form = ASFW::AVC::Cmd::StreamFormatSubfunction::kList,
                            .opcode = ASFW::AVC::Cmd::StreamFormatOpcode::kStreamFormatSupport,
                            .plug = ASFW::AVC::Cmd::PlugAddress::UnitPlug(dir, ASFW::AVC::Cmd::UnitPlugType::kPcr, 0),
                            .index = request.index,
                        },
                    },
                    [self, request](ASFW::AVC::Expected<ASFW::AVC::Cmd::StreamFormatReply> res) {
                        if (!res) {
                            ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s stream-format list ended at entry %u GUID=0x%016llx",
                                     DirectionName(request.direction), static_cast<unsigned>(request.index), self->guid_);
                        } else {
                            self->HandleFormation(request, *res);
                        }
                        self->SubmitNext();
                    });
                break;
            }
            case ReadOnlyProbeCommand::kChannelPositions: {
                const auto dir = request.direction == PlugDirection::kInput ?
                    ASFW::AVC::Cmd::PlugDirection::kInput : ASFW::AVC::Cmd::PlugDirection::kOutput;
                unit_.Status(
                    ASFW::AVC::BridgeCo::ExtendedPlugInfoCommand{
                        .operands = ASFW::AVC::BridgeCo::ExtendedPlugInfoOperands{
                            .plug = ASFW::AVC::Cmd::PlugAddress::UnitPlug(dir, ASFW::AVC::Cmd::UnitPlugType::kPcr, 0),
                            .type = ASFW::AVC::BridgeCo::InfoType::kChannelPositions,
                        },
                    },
                    [self, request](ASFW::AVC::Expected<ASFW::AVC::BridgeCo::ExtendedPlugInfoReply> res) {
                        if (!res) {
                            self->HandleError(request, res.error());
                        } else {
                            self->HandlePositions(request, res->Data());
                        }
                        self->SubmitNext();
                    });
                break;
            }
            case ReadOnlyProbeCommand::kSectionType: {
                const auto dir = request.direction == PlugDirection::kInput ?
                    ASFW::AVC::Cmd::PlugDirection::kInput : ASFW::AVC::Cmd::PlugDirection::kOutput;
                unit_.Status(
                    ASFW::AVC::BridgeCo::ExtendedPlugInfoCommand{
                        .operands = ASFW::AVC::BridgeCo::ExtendedPlugInfoOperands{
                            .plug = ASFW::AVC::Cmd::PlugAddress::UnitPlug(dir, ASFW::AVC::Cmd::UnitPlugType::kPcr, 0),
                            .type = ASFW::AVC::BridgeCo::InfoType::kClusterInfo,
                            .extra = static_cast<uint8_t>(request.index + 1U),
                        },
                    },
                    [self, request](ASFW::AVC::Expected<ASFW::AVC::BridgeCo::ExtendedPlugInfoReply> res) {
                        if (!res) {
                            self->HandleError(request, res.error());
                        } else {
                            // The reply echoes the 1-based section id, then the
                            // section type (Linux bebob_command.c:228, :246).
                            const auto portType = res->AsClusterPortType(
                                static_cast<uint8_t>(request.index + 1U));
                            if (!portType) {
                                self->HandleError(request, portType.error());
                            } else {
                                const auto value = static_cast<uint8_t>(*portType);
                                if (request.index < self->Plug(request.direction).channelSections.size()) {
                                    self->Plug(request.direction).channelSections[request.index].type = value;
                                }
                                ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s section %u type=0x%02x GUID=0x%016llx",
                                         DirectionName(request.direction), static_cast<unsigned>(request.index), value, self->guid_);
                            }
                        }
                        self->SubmitNext();
                    });
                break;
            }
        }
    }

    void AddFullInventory() {
        for (const PlugDirection direction : {PlugDirection::kInput, PlugDirection::kOutput}) {
            queue_.push_back({ReadOnlyProbeCommand::kIsochPlugType, direction});
            queue_.push_back({ReadOnlyProbeCommand::kStreamFormatList, direction});
            // The channel positions give the device's AM824 slot order (Linux
            // bebob_stream.c map_data_channels :254-372). Without them the
            // channel map stays identity, which is wrong for planar devices
            // such as the Phase 88.
            queue_.push_back({ReadOnlyProbeCommand::kChannelPositions, direction});
        }
    }

    void HandleError(const Request& request, const ASFW::AVC::AvcError& err) {
        ASFW_LOG(AVC, "BeBoBProbe: %{public}s %{public}s unavailable result=%u GUID=0x%016llx",
                 RequestName(request.command), DirectionName(request.direction),
                 static_cast<unsigned>(err.kind), guid_);
        if (!unitPlugCountsComplete_) {
            ASFW_LOG(AVC,
                     "BeBoBProbe: generic PLUG_INFO unavailable; stopping inventory GUID=0x%016llx",
                     guid_);
            queue_.clear();
            next_ = 0;
        }
    }

    void HandleUnitPlugCounts(const ASFW::AVC::Cmd::PlugInfoReply& reply) {
        model_.unitPlugCounts = reply.unit;
        unitPlugCountsComplete_ = true;
        const auto& counts = *model_.unitPlugCounts;
        ASFW_LOG(AVC,
                 "BeBoBProbe: generic PLUG_INFO ISO in=%u out=%u ext in=%u out=%u GUID=0x%016llx",
                 static_cast<unsigned>(counts.isochronousInputs),
                 static_cast<unsigned>(counts.isochronousOutputs),
                 static_cast<unsigned>(counts.externalInputs),
                 static_cast<unsigned>(counts.externalOutputs), guid_);
        if (counts.isochronousInputs == 0 || counts.isochronousOutputs == 0) {
            ASFW_LOG(AVC, "BeBoBProbe: no duplex ISO plug pair; stopping inventory GUID=0x%016llx", guid_);
            return;
        }
        AddFullInventory();
    }

    void HandleFormation(const Request& request, const ASFW::AVC::Cmd::StreamFormatReply& reply) {
        if (reply.index != request.index) {
            ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s stream-format list ended at entry %u GUID=0x%016llx",
                     DirectionName(request.direction), static_cast<unsigned>(request.index), guid_);
            return;
        }
        std::optional<StreamFormation> formation;
        if (reply.format.kind == ASFW::AVC::Cmd::StreamFormat::Kind::kCompoundAm824) {
            formation = StreamFormation{
                .rateCode = static_cast<uint8_t>(reply.format.compound.rate),
                .pcmChannels = static_cast<uint8_t>(reply.format.compound.PcmChannels()),
                .midiSlots = static_cast<uint8_t>(reply.format.compound.MidiChannels()),
            };
        } else {
            formation = ParseStreamFormation(reply.format.Raw());
        }
        if (!formation.has_value()) {
            ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s stream-format entry %u malformed/unsupported GUID=0x%016llx",
                     DirectionName(request.direction), static_cast<unsigned>(request.index), guid_);
            return;
        }
        ASFW_LOG(AVC,
                 "BeBoBProbe: ISO %{public}s format[%u] rateCode=0x%02x pcm=%u midiSlots=%u dbs=%u GUID=0x%016llx",
                 DirectionName(request.direction), static_cast<unsigned>(request.index), formation->rateCode,
                 static_cast<unsigned>(formation->pcmChannels), static_cast<unsigned>(formation->midiSlots),
                 static_cast<unsigned>(formation->pcmChannels + formation->midiSlots), guid_);
        Plug(request.direction).supportedFormations.push_back(*formation);
        if (request.index + 1U < kMaxFormatEntries) {
            queue_.push_back({ReadOnlyProbeCommand::kStreamFormatList, request.direction,
                              static_cast<uint8_t>(request.index + 1U)});
        }
    }

    void HandlePositions(const Request& request, std::span<const uint8_t> payload) {
        const auto sections = ParseChannelPositionSections(payload);
        if (!sections.has_value()) {
            ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s channel-map malformed GUID=0x%016llx",
                     DirectionName(request.direction), guid_);
            return;
        }
        ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s channel-map sections=%u bytes=%zu GUID=0x%016llx",
                 DirectionName(request.direction), static_cast<unsigned>(sections->size()), payload.size(), guid_);
        if (sections->size() > kMaxSections) return;
        Plug(request.direction).channelSections = std::move(*sections);
        for (uint8_t section = 0; section < Plug(request.direction).channelSections.size(); ++section) {
            queue_.push_back({ReadOnlyProbeCommand::kSectionType, request.direction, section});
        }
    }

    [[nodiscard]] IsochronousPlugModel& Plug(PlugDirection direction) noexcept {
        return direction == PlugDirection::kInput ? model_.input : model_.output;
    }

    ASFW::AVC::IAvcUnit& unit_;
    uint64_t guid_{0};
    std::vector<Request> queue_{};
    size_t next_{0};
    bool unitPlugCountsComplete_{false};
    DeviceModel model_{};
    ReadOnlyProbeCompletion completion_{};
};

} // namespace

std::optional<StreamFormation>
ParseStreamFormation(std::span<const uint8_t> formation) noexcept {
    if (formation.size() < 5 || formation[0] != 0x90 || formation[1] != 0x40) return std::nullopt;
    const size_t fields = formation[4];
    if (fields > (formation.size() - 5U) / 2U) return std::nullopt;
    StreamFormation result{.rateCode = formation[2]};
    for (size_t index = 0; index < fields; ++index) {
        const uint8_t channels = formation[5 + index * 2];
        uint8_t& total = formation[6 + index * 2] == 0x0d ? result.midiSlots : result.pcmChannels;
        const uint8_t format = formation[6 + index * 2];
        if ((format != 0x00 && format != 0x06 && format != 0x0d) ||
            static_cast<uint16_t>(total) + channels > 0xffU) {
            return std::nullopt;
        }
        total = static_cast<uint8_t>(total + channels);
    }
    return result;
}

std::optional<StreamFormation>
ParseExtendedStreamFormatListResponse(uint8_t requestedIndex,
                                      std::span<const uint8_t> operands) noexcept {
    if (operands.size() < 9 || operands[7] != requestedIndex) {
        return std::nullopt;
    }
    return ParseStreamFormation(operands.subspan(8));
}

std::optional<CurrentStreamFormat>
ParseExtendedStreamFormatSingleResponse(std::span<const uint8_t> operands) noexcept {
    if (operands.size() < 7 || operands[0] != kExtendedPlugInfo) return std::nullopt;
    CurrentStreamFormat result{};
    switch (operands[6]) {
        case static_cast<uint8_t>(StreamFormatState::kActive): result.state = StreamFormatState::kActive; break;
        case static_cast<uint8_t>(StreamFormatState::kInactive): result.state = StreamFormatState::kInactive; break;
        case static_cast<uint8_t>(StreamFormatState::kNoStreamFormat): result.state = StreamFormatState::kNoStreamFormat; return result;
        default: return std::nullopt;
    }
    const auto formation = ParseStreamFormation(operands.subspan(7));
    if (!formation.has_value()) return std::nullopt;
    result.formation = *formation;
    return result;
}

std::optional<std::vector<ChannelSection>>
ParseChannelPositionSections(std::span<const uint8_t> payload) noexcept {
    if (payload.empty() || payload[0] > kMaxSections) return std::nullopt;
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

bool DeviceModel::HasAgreedCurrentRate() const noexcept {
    return CurrentRateCode().has_value();
}

std::optional<uint8_t> DeviceModel::CurrentRateCode() const noexcept {
    const auto inputRate = input.currentFormat.has_value() ? input.currentFormat->formation : std::nullopt;
    const auto outputRate = output.currentFormat.has_value() ? output.currentFormat->formation : std::nullopt;
    if (!inputRate.has_value() || !outputRate.has_value() || inputRate->rateCode != outputRate->rateCode) {
        return std::nullopt;
    }
    return inputRate->rateCode;
}

bool DeviceModel::SupportsDuplexFormation(uint8_t pcmChannels,
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

void StartBeBoBPlug0Discovery(ASFW::AVC::IAvcUnit& unit, uint64_t guid,
                              ReadOnlyProbeCompletion completion) {
    std::make_shared<Probe>(unit, guid, std::move(completion))->Start();
}

} // namespace ASFW::Audio::BeBoB
