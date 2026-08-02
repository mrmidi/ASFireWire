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

using ::ASFW::Protocols::AVC::IAVCCommandSubmitter;
using ::ASFW::Protocols::AVC::AVCCdb;
using ::ASFW::Protocols::AVC::AVCResult;
using ::ASFW::Protocols::AVC::AVCCompletion;

#include <memory>
#include <utility>
#include <vector>

namespace ASFW::Audio::BeBoB {
namespace {

constexpr uint8_t kOpcodePlugInfo = 0x02;
constexpr uint8_t kOpcodeStreamFormatSupport = 0x2f;
constexpr uint8_t kExtendedPlugInfo = 0xc0;
constexpr uint8_t kExtendedFormatList = 0xc1;
constexpr uint8_t kInfoPlugType = 0x00;
constexpr uint8_t kInfoChannelPosition = 0x03;
constexpr uint8_t kInfoSection = 0x07;
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
    Probe(IAVCCommandSubmitter& submitter, uint64_t guid, ReadOnlyProbeCompletion completion)
        : submitter_(submitter), guid_(guid), completion_(std::move(completion)) {
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
        submitter_.SubmitCommand(BuildReadOnlyProbeCommand(request.command, request.direction, request.index),
                                 [self, request](AVCResult result, const AVCCdb& response) {
            self->HandleResponse(request, result, response);
            self->SubmitNext();
        });
    }

    void AddFullInventory() {
        for (const PlugDirection direction : {PlugDirection::kInput, PlugDirection::kOutput}) {
            queue_.push_back({ReadOnlyProbeCommand::kIsochPlugType, direction});
            queue_.push_back({ReadOnlyProbeCommand::kStreamFormatList, direction});
        }
    }

    void HandleResponse(const Request& request, AVCResult result, const AVCCdb& response) {
        if (!IsSuccess(result)) {
            ASFW_LOG(AVC, "BeBoBProbe: %{public}s %{public}s unavailable result=%u GUID=0x%016llx",
                     RequestName(request.command), DirectionName(request.direction),
                     static_cast<unsigned>(result), guid_);
            if (!unitPlugCountsComplete_) {
                ASFW_LOG(AVC,
                         "BeBoBProbe: generic PLUG_INFO unavailable; stopping inventory GUID=0x%016llx",
                         guid_);
                queue_.clear();
                next_ = 0;
            }
            return;
        }
        if (request.command == ReadOnlyProbeCommand::kUnitPlugCounts) {
            HandleUnitPlugCounts(response);
            return;
        }
        if (request.command == ReadOnlyProbeCommand::kStreamFormatList) {
            HandleFormation(request, response);
            return;
        }
        if (response.operandLength < 8) {
            ASFW_LOG(AVC, "BeBoBProbe: short %{public}s response (%zu operands) GUID=0x%016llx",
                     RequestName(request.command), response.operandLength, guid_);
            return;
        }
        const uint8_t value = response.operands[7];
        switch (request.command) {
            case ReadOnlyProbeCommand::kIsochPlugType:
                Plug(request.direction).plugType = value;
                ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s plug 0 type=0x%02x GUID=0x%016llx",
                         DirectionName(request.direction), value, guid_);
                break;
            case ReadOnlyProbeCommand::kChannelPositions: HandlePositions(request, response); break;
            case ReadOnlyProbeCommand::kSectionType:
                if (request.index < Plug(request.direction).channelSections.size()) {
                    Plug(request.direction).channelSections[request.index].type = value;
                }
                ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s section %u type=0x%02x GUID=0x%016llx",
                         DirectionName(request.direction), static_cast<unsigned>(request.index), value, guid_);
                break;
            case ReadOnlyProbeCommand::kUnitPlugCounts:
            case ReadOnlyProbeCommand::kStreamFormatList: break;
        }
    }

    void HandleUnitPlugCounts(const AVCCdb& response) {
        // Standard unit PLUG_INFO is an 8-byte CDB. Its four result bytes are
        // operands 1..4: ISO input/output, external input/output.
        if (response.operandLength < 5) {
            ASFW_LOG(AVC, "BeBoBProbe: short generic PLUG_INFO response (%zu operands) GUID=0x%016llx",
                     response.operandLength, guid_);
            return;
        }
        model_.unitPlugCounts = UnitPlugCounts{
            .isochronousInputs = response.operands[1],
            .isochronousOutputs = response.operands[2],
            .externalInputs = response.operands[3],
            .externalOutputs = response.operands[4],
        };
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

    void HandleFormation(const Request& request, const AVCCdb& response) {
        if (response.operandLength < 9 || response.operands[7] != request.index) {
            ASFW_LOG(AVC, "BeBoBProbe: ISO %{public}s stream-format list ended at entry %u GUID=0x%016llx",
                     DirectionName(request.direction), static_cast<unsigned>(request.index), guid_);
            return;
        }
        const auto formation = ParseExtendedStreamFormatListResponse(
            request.index, std::span<const uint8_t>{response.operands.data(), response.operandLength});
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

    void HandlePositions(const Request& request, const AVCCdb& response) {
        const std::span<const uint8_t> payload{response.operands.data() + 7,
                                                response.operandLength - 7};
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

    IAVCCommandSubmitter& submitter_;
    uint64_t guid_{0};
    std::vector<Request> queue_{};
    size_t next_{0};
    bool unitPlugCountsComplete_{false};
    DeviceModel model_{};
    ReadOnlyProbeCompletion completion_{};
};

} // namespace

AVCCdb BuildReadOnlyProbeCommand(ReadOnlyProbeCommand command,
                                 PlugDirection direction, uint8_t index) noexcept {
    AVCCdb cdb{};
    cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);
    cdb.subunit = kAVCSubunitUnit;

    if (command == ReadOnlyProbeCommand::kUnitPlugCounts) {
        cdb.opcode = kOpcodePlugInfo;
        cdb.operands[0] = 0x00;
        cdb.operandLength = 5;
        return cdb;
    }

    cdb.operands[1] = static_cast<uint8_t>(direction);
    cdb.operands[2] = 0x00;
    cdb.operands[3] = 0x00;
    cdb.operands[4] = 0x00;
    cdb.operands[5] = 0xff;

    if (command == ReadOnlyProbeCommand::kStreamFormatList) {
        cdb.opcode = kOpcodeStreamFormatSupport;
        cdb.operands[0] = kExtendedFormatList;
        cdb.operands[6] = 0xff;
        cdb.operands[7] = index;
        cdb.operandLength = 8;
        return cdb;
    }

    cdb.opcode = kOpcodePlugInfo;
    cdb.operands[0] = kExtendedPlugInfo;
    switch (command) {
        case ReadOnlyProbeCommand::kIsochPlugType: cdb.operands[6] = kInfoPlugType; break;
        case ReadOnlyProbeCommand::kChannelPositions: cdb.operands[6] = kInfoChannelPosition; break;
        case ReadOnlyProbeCommand::kSectionType:
            cdb.operands[6] = kInfoSection;
            cdb.operands[7] = static_cast<uint8_t>(index + 1U);
            break;
        case ReadOnlyProbeCommand::kUnitPlugCounts:
        case ReadOnlyProbeCommand::kStreamFormatList: break;
    }
    cdb.operandLength = command == ReadOnlyProbeCommand::kSectionType ? 8 : 7;
    return cdb;
}

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

void StartBeBoBPlug0Discovery(IAVCCommandSubmitter& submitter, uint64_t guid,
                              ReadOnlyProbeCompletion completion) {
    std::make_shared<Probe>(submitter, guid, std::move(completion))->Start();
}

} // namespace ASFW::Audio::BeBoB
