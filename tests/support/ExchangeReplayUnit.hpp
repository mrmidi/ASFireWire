// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ExchangeReplayUnit.hpp - An AV/C unit that answers from an exported exchange log.
//
// Replay runs the real discovery session (the same reducer and descriptor
// operation as attach) against a recorded conversation: each submitted frame
// takes the first unused record with the same command bytes and gets that
// record's outcome. A frame the log never saw is answered NOT IMPLEMENTED and
// counted, so a replay that diverges from the capture is visible.

#pragma once

#include "ASFWDriver/Protocols/AVC/Core/IAvcUnit.hpp"
#include "ASFWDriver/Protocols/AVC/FcpExchangeRecorder.hpp"

#include <algorithm>
#include <vector>

namespace ASFW::AVC::Testing {

class ExchangeReplayUnit final : public IAvcUnit {
public:
    ExchangeReplayUnit(Protocols::AVC::FcpExchangeLog log, uint64_t guid, FW::NodeId node, FW::Generation generation)
        : log_(std::move(log)), used_(log_.records.size(), false), guid_(guid), node_(node), generation_(generation) {}

    void Submit(const CommandFrame& frame, FW::Generation, ResponseCallback completion) override {
        const auto wire = frame.WireBytes();
        for (size_t i = 0; i < log_.records.size(); ++i) {
            const auto& record = log_.records[i];
            if (used_[i] || !std::ranges::equal(record.command, wire)) continue;
            used_[i] = true;
            ++replayed_;
            completion(Answer(frame, record));
            return;
        }
        unmatched_.emplace_back(wire.begin(), wire.end());
        const uint8_t notImplemented[3]{0x08, wire.size() > 1 ? wire[1] : uint8_t{0xFF},
                                        wire.size() > 2 ? wire[2] : uint8_t{0}};
        completion(ParseResponseFor(frame, notImplemented));
    }
    [[nodiscard]] FW::NodeId NodeId() const noexcept override { return node_; }
    [[nodiscard]] FW::Generation CurrentGeneration() const noexcept override { return generation_; }
    [[nodiscard]] uint64_t Guid() const noexcept override { return guid_; }

    [[nodiscard]] size_t Replayed() const noexcept { return replayed_; }
    /// Recorded exchanges the replay never asked for (0: the whole capture replayed).
    [[nodiscard]] size_t Unused() const noexcept { return log_.records.size() - replayed_; }
    [[nodiscard]] const std::vector<std::vector<uint8_t>>& Unmatched() const noexcept { return unmatched_; }

private:
    static Expected<Response> Answer(const CommandFrame& frame, const Protocols::AVC::FcpExchangeRecord& record) {
        using O = Protocols::AVC::FcpExchangeOutcome;
        switch (record.outcome) {
            case O::kResponse: return ParseResponseFor(frame, record.response);
            case O::kTimeout: return Fail(AvcErrorKind::kTimeout);
            case O::kBusReset: return Fail(AvcErrorKind::kBusReset);
            case O::kRefusedByFilter: return Fail(AvcErrorKind::kRefused);
            case O::kBusy: return Fail(AvcErrorKind::kBusy);
            case O::kResponseMismatch:
            case O::kTransportError:
            case O::kInvalid: return Fail(AvcErrorKind::kTransportError);
        }
        return Fail(AvcErrorKind::kTransportError);
    }

    Protocols::AVC::FcpExchangeLog log_;
    std::vector<bool> used_;
    std::vector<std::vector<uint8_t>> unmatched_;
    size_t replayed_{0};
    uint64_t guid_;
    FW::NodeId node_;
    FW::Generation generation_;
};

} // namespace ASFW::AVC::Testing
