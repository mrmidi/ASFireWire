// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Wire behavior cross-validated with Linux sound/firewire/bebob/
// bebob_command.c:91-107, 289-328 and bebob_stream.c:254-370, 705-820.
// This is a fresh implementation; reference code was not copied.

#include "BridgeCoReadOnlyProbe.hpp"

#include "../../../Logging/Logging.hpp"

#include <memory>
#include <vector>

namespace ASFW::Protocols::AVC::BridgeCo {
namespace {

constexpr uint8_t kOpcodePlugInfo = 0x02;
constexpr uint8_t kOpcodeStreamFormatSupport = 0x2f;
constexpr uint8_t kExtendedPlugInfo = 0xc0;
constexpr uint8_t kExtendedFormatList = 0xc1;
constexpr uint8_t kInfoPlugType = 0x00;
constexpr uint8_t kInfoChannelCount = 0x02;
constexpr uint8_t kInfoChannelPosition = 0x03;
constexpr uint8_t kInfoSection = 0x07;
constexpr uint8_t kInput = 0x00;
constexpr uint8_t kOutput = 0x01;
constexpr uint8_t kMaxFormatEntries = 8;
constexpr uint8_t kMaxSections = 16;

enum class RequestKind : uint8_t { kType, kChannels, kFormat, kPositions, kSection };
struct Request { RequestKind kind; uint8_t direction; uint8_t index{0}; };

[[nodiscard]] const char* DirectionName(uint8_t direction) noexcept {
    return direction == kInput ? "input" : "output";
}

[[nodiscard]] const char* RequestName(RequestKind kind) noexcept {
    switch (kind) {
        case RequestKind::kType: return "plug type";
        case RequestKind::kChannels: return "channel count";
        case RequestKind::kFormat: return "stream format";
        case RequestKind::kPositions: return "channel map";
        case RequestKind::kSection: return "section type";
    }
    return "unknown";
}

[[nodiscard]] AVCCdb BuildRequest(const Request& request) noexcept {
    AVCCdb cdb{};
    cdb.ctype = static_cast<uint8_t>(AVCCommandType::kStatus);
    cdb.subunit = kAVCSubunitUnit;
    cdb.operands[1] = request.direction;
    cdb.operands[2] = 0x00; // Unit plug addressing mode.
    cdb.operands[3] = 0x00; // Isochronous unit plug class.
    cdb.operands[4] = 0x00; // Plug 0.
    cdb.operands[5] = 0xff; // Reserved.
    if (request.kind == RequestKind::kFormat) {
        cdb.opcode = kOpcodeStreamFormatSupport;
        cdb.operands[0] = kExtendedFormatList;
        cdb.operands[6] = request.index;
        cdb.operandLength = 7;
        return cdb;
    }

    cdb.opcode = kOpcodePlugInfo;
    cdb.operands[0] = kExtendedPlugInfo;
    switch (request.kind) {
        case RequestKind::kType: cdb.operands[6] = kInfoPlugType; break;
        case RequestKind::kChannels: cdb.operands[6] = kInfoChannelCount; break;
        case RequestKind::kPositions: cdb.operands[6] = kInfoChannelPosition; break;
        case RequestKind::kSection:
            cdb.operands[6] = kInfoSection;
            cdb.operands[7] = static_cast<uint8_t>(request.index + 1U);
            break;
        case RequestKind::kFormat: break;
    }
    cdb.operandLength = request.kind == RequestKind::kSection ? 8 : 7;
    return cdb;
}

class Probe final : public std::enable_shared_from_this<Probe> {
public:
    Probe(IAVCCommandSubmitter& submitter, uint64_t guid) : submitter_(submitter), guid_(guid) {
        // The first request is also FFADO's generic BeBoB discriminator
        // (libffado-2.5.0/src/bebob/bebob_avdevice.cpp:93-124). If the FCP
        // route is dead, stop here instead of producing a long retry storm.
        queue_.push_back({RequestKind::kChannels, kInput});
    }

    void Start() {
        ASFW_LOG(AVC,
                 "BeBoBProbe: PHASE 88 matched; starting STATUS-only BridgeCo inventory GUID=0x%016llx",
                 guid_);
        SubmitNext();
    }

private:
    void SubmitNext() {
        if (next_ == queue_.size()) {
            ASFW_LOG(AVC, "BeBoBProbe: inventory complete GUID=0x%016llx", guid_);
            return;
        }
        const Request request = queue_[next_++];
        auto self = shared_from_this();
        submitter_.SubmitCommand(BuildRequest(request), [self, request](AVCResult result,
                                                                          const AVCCdb& response) {
            self->HandleResponse(request, result, response);
            self->SubmitNext();
        });
    }

    void AddFullInventory() {
        for (const uint8_t direction : {kInput, kOutput}) {
            queue_.push_back({RequestKind::kType, direction});
            queue_.push_back({RequestKind::kChannels, direction});
            queue_.push_back({RequestKind::kFormat, direction});
            queue_.push_back({RequestKind::kPositions, direction});
        }
    }

    void HandleResponse(const Request& request, AVCResult result, const AVCCdb& response) {
        if (!IsSuccess(result)) {
            ASFW_LOG(AVC, "BeBoBProbe: %s %s unavailable result=%u GUID=0x%016llx",
                     RequestName(request.kind), DirectionName(request.direction),
                     static_cast<unsigned>(result), guid_);
            if (!discriminatorComplete_) {
                ASFW_LOG(AVC,
                         "BeBoBProbe: stopping after discriminator failure; avoiding FCP retry storm GUID=0x%016llx",
                         guid_);
                queue_.clear();
                next_ = 0;
            }
            return;
        }
        if (!discriminatorComplete_) {
            discriminatorComplete_ = true;
            AddFullInventory();
        }
        if (request.kind == RequestKind::kFormat) {
            HandleFormation(request, response);
            return;
        }
        if (response.operandLength < 8) {
            ASFW_LOG(AVC, "BeBoBProbe: short %s response (%zu operands) GUID=0x%016llx",
                     RequestName(request.kind), response.operandLength, guid_);
            return;
        }
        const uint8_t value = response.operands[7];
        switch (request.kind) {
            case RequestKind::kType:
                ASFW_LOG(AVC, "BeBoBProbe: ISO %s plug 0 type=0x%02x GUID=0x%016llx",
                         DirectionName(request.direction), value, guid_);
                break;
            case RequestKind::kChannels:
                ASFW_LOG(AVC, "BeBoBProbe: ISO %s plug 0 channels=%u GUID=0x%016llx",
                         DirectionName(request.direction), static_cast<unsigned>(value), guid_);
                break;
            case RequestKind::kPositions: HandlePositions(request, response); break;
            case RequestKind::kSection:
                ASFW_LOG(AVC, "BeBoBProbe: ISO %s section %u type=0x%02x GUID=0x%016llx",
                         DirectionName(request.direction), static_cast<unsigned>(request.index), value, guid_);
                break;
            case RequestKind::kFormat: break;
        }
    }

    void HandleFormation(const Request& request, const AVCCdb& response) {
        if (response.operandLength < 9 || response.operands[7] != request.index) {
            ASFW_LOG(AVC, "BeBoBProbe: ISO %s stream-format list ended at entry %u GUID=0x%016llx",
                     DirectionName(request.direction), static_cast<unsigned>(request.index), guid_);
            return;
        }
        const auto formation = ParseExtendedStreamFormatListResponse(
            request.index, std::span<const uint8_t>{response.operands.data(), response.operandLength});
        if (!formation.has_value()) {
            ASFW_LOG(AVC, "BeBoBProbe: ISO %s stream-format entry %u malformed/unsupported GUID=0x%016llx",
                     DirectionName(request.direction), static_cast<unsigned>(request.index), guid_);
            return;
        }
        ASFW_LOG(AVC,
                 "BeBoBProbe: ISO %s format[%u] rateCode=0x%02x pcm=%u midiSlots=%u dbs=%u GUID=0x%016llx",
                 DirectionName(request.direction), static_cast<unsigned>(request.index), formation->rateCode,
                 static_cast<unsigned>(formation->pcmChannels), static_cast<unsigned>(formation->midiSlots),
                 static_cast<unsigned>(formation->pcmChannels + formation->midiSlots), guid_);
        if (request.index + 1U < kMaxFormatEntries) {
            queue_.push_back({RequestKind::kFormat, request.direction,
                              static_cast<uint8_t>(request.index + 1U)});
        }
    }

    void HandlePositions(const Request& request, const AVCCdb& response) {
        const std::span<const uint8_t> payload{response.operands.data() + 7,
                                                response.operandLength - 7};
        if (payload.empty()) return;
        const uint8_t sections = payload[0];
        ASFW_LOG(AVC, "BeBoBProbe: ISO %s channel-map sections=%u bytes=%zu GUID=0x%016llx",
                 DirectionName(request.direction), static_cast<unsigned>(sections), payload.size(), guid_);
        if (sections > kMaxSections) return;
        for (uint8_t section = 0; section < sections; ++section) {
            queue_.push_back({RequestKind::kSection, request.direction, section});
        }
    }

    IAVCCommandSubmitter& submitter_;
    uint64_t guid_{0};
    std::vector<Request> queue_{};
    size_t next_{0};
    bool discriminatorComplete_{false};
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
    // BridgeCo's response layout is validated against
    // alsa-userspace-control-protocols-impl/protocols/bebob/src/bridgeco.rs:1703-1764
    // and Linux sound/firewire/bebob/bebob_command.c:289-328.  No source is copied.
    if (operands.size() < 9 || operands[7] != requestedIndex) {
        return std::nullopt;
    }
    return ParseStreamFormation(operands.subspan(8));
}

void StartPhase88ReadOnlyProbe(IAVCCommandSubmitter& submitter, uint64_t guid) {
    std::make_shared<Probe>(submitter, guid)->Start();
}

} // namespace ASFW::Protocols::AVC::BridgeCo
