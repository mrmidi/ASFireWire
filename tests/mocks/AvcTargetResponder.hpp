// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcTargetResponder.hpp — Models the AV/C target side of an FCP exchange so
// protocol tests can run against the real FCPTransport instead of replacing it.
//
// FCP is asymmetric on the wire: the initiator's command is a block write to
// kFCPCommandAddress, and the target's response is a *separate* block write
// back to the initiator's kFCPResponseAddress. The response therefore reaches
// the transport through FCPTransport::OnFCPResponse (driven by the async RX
// router in production), never through the command write's own completion.
// This responder reproduces exactly that split.
//
// Host-test only.

#pragma once

#include "ASFWDriver/Protocols/AVC/AVCDefs.hpp"
#include "ASFWDriver/Protocols/AVC/FCPTransport.hpp"

#include "DeferredFireWireBus.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::Testing {

/// One scripted target answer to a single AV/C command.
struct AvcReply {
    enum class Kind : uint8_t {
        kEcho,          ///< Echo the command back with the response code substituted.
        kRaw,           ///< Send caller-supplied bytes verbatim.
        kNone,          ///< Send nothing, so the transport's timeout is what fires.
        kWriteFailure,  ///< The command write never reaches the target.
    };

    Kind kind{Kind::kEcho};
    uint8_t responseCode{static_cast<uint8_t>(Protocols::AVC::AVCResponseType::kAccepted)};
    std::vector<uint8_t> raw{};
    std::optional<size_t> truncateTo{};
    std::vector<std::pair<size_t, uint8_t>> patches{};

    [[nodiscard]] static AvcReply Accepted() { return WithCode(Protocols::AVC::AVCResponseType::kAccepted); }
    [[nodiscard]] static AvcReply ImplementedStable() {
        return WithCode(Protocols::AVC::AVCResponseType::kImplementedStable);
    }
    [[nodiscard]] static AvcReply Rejected() { return WithCode(Protocols::AVC::AVCResponseType::kRejected); }
    [[nodiscard]] static AvcReply NotImplemented() {
        return WithCode(Protocols::AVC::AVCResponseType::kNotImplemented);
    }
    [[nodiscard]] static AvcReply Interim() { return WithCode(Protocols::AVC::AVCResponseType::kInterim); }

    [[nodiscard]] static AvcReply NoResponse() {
        AvcReply reply{};
        reply.kind = Kind::kNone;
        return reply;
    }

    /// The command block write itself fails, which the transport reports as
    /// FCPStatus::kTransportError rather than a timeout.
    [[nodiscard]] static AvcReply WriteFailure() {
        AvcReply reply{};
        reply.kind = Kind::kWriteFailure;
        return reply;
    }

    /// A frame shorter than kAVCFrameMinSize, which the transport must reject.
    [[nodiscard]] static AvcReply ShortFrame(size_t bytes = Protocols::AVC::kAVCFrameMinSize - 1) {
        AvcReply reply = Accepted();
        reply.truncateTo = bytes;
        return reply;
    }

    [[nodiscard]] static AvcReply RawBytes(std::vector<uint8_t> bytes) {
        AvcReply reply{};
        reply.kind = Kind::kRaw;
        reply.raw = std::move(bytes);
        return reply;
    }

    /// Corrupt the 3-byte OUI a VENDOR-DEPENDENT response echoes back at
    /// operand offset 0 (frame bytes 3..5).
    [[nodiscard]] AvcReply&& WithWrongOui() && {
        patches.emplace_back(3U, 0xFFU);
        patches.emplace_back(4U, 0xFFU);
        patches.emplace_back(5U, 0xFFU);
        return std::move(*this);
    }

    [[nodiscard]] AvcReply&& WithPatch(size_t index, uint8_t value) && {
        patches.emplace_back(index, value);
        return std::move(*this);
    }

    /// Overwrite the operand region (frame bytes 3..) of the echoed command.
    [[nodiscard]] AvcReply&& WithOperands(std::span<const uint8_t> operands) && {
        for (size_t i = 0; i < operands.size(); ++i) {
            patches.emplace_back(3U + i, operands[i]);
        }
        return std::move(*this);
    }

private:
    [[nodiscard]] static AvcReply WithCode(Protocols::AVC::AVCResponseType type) {
        AvcReply reply{};
        reply.kind = Kind::kEcho;
        reply.responseCode = static_cast<uint8_t>(type);
        return reply;
    }
};

/// Drains a DeferredFireWireBus, answering every write aimed at the FCP command
/// address as an AV/C target would.
///
/// Resolution order for each command: an explicitly scripted reply first (that
/// is per-test error injection and must win), then the installed device model,
/// then the configured default.
class AvcTargetResponder {
public:
    using DeviceModel = std::function<std::optional<AvcReply>(std::span<const uint8_t> command)>;

    AvcTargetResponder(Async::Testing::DeferredFireWireBus& bus,
                       std::shared_ptr<Protocols::AVC::FCPTransport> transport,
                       uint16_t srcNodeId,
                       uint32_t generation)
        : bus_(bus),
          transport_(std::move(transport)),
          srcNodeId_(srcNodeId),
          generation_(generation) {}

    void Script(AvcReply reply) { scripted_.push_back(std::move(reply)); }
    void SetDeviceModel(DeviceModel model) { deviceModel_ = std::move(model); }
    void SetDefaultReply(AvcReply reply) { defaultReply_ = std::move(reply); }

    [[nodiscard]] const std::vector<Protocols::AVC::FCPFrame>& Commands() const noexcept {
        return commands_;
    }
    [[nodiscard]] size_t CommandCount() const noexcept { return commands_.size(); }
    [[nodiscard]] size_t ScriptedRemaining() const noexcept { return scripted_.size(); }

    void ClearCommands() noexcept { commands_.clear(); }

    /// Complete the oldest pending bus write and, if it was an FCP command,
    /// deliver the target's response. Returns false when nothing was pending.
    bool Step() {
        if (bus_.PendingWriteCount() == 0) {
            return false;
        }

        const auto summary = bus_.PendingWriteAt(0);
        const bool isFcpCommand = FullAddress(summary.address) == Protocols::AVC::kFCPCommandAddress;

        std::optional<AvcReply> reply;
        if (isFcpCommand) {
            reply = Resolve(summary.data);
            Protocols::AVC::FCPFrame frame{};
            frame.length = std::min(summary.data.size(), frame.data.size());
            std::copy_n(summary.data.begin(), frame.length, frame.data.begin());
            commands_.push_back(frame);
        }

        const bool writeFails = reply.has_value() && reply->kind == AvcReply::Kind::kWriteFailure;
        if (!bus_.CompleteNextWrite(writeFails ? Async::AsyncStatus::kTimeout
                                               : Async::AsyncStatus::kSuccess)) {
            return false;
        }

        if (isFcpCommand && !writeFails && reply.has_value()) {
            DeliverResponse(summary.data, *reply);
        }
        return true;
    }

    /// Step until the bus is quiet. Commands chained from a completion callback
    /// enqueue new writes, so this settles a whole AV/C sequence.
    size_t Drain(size_t maxSteps = 256) {
        size_t steps = 0;
        while (steps < maxSteps && Step()) {
            ++steps;
        }
        return steps;
    }

private:
    [[nodiscard]] static uint64_t FullAddress(const Async::FWAddress& address) noexcept {
        return (static_cast<uint64_t>(address.addressHi) << 32U) |
               static_cast<uint64_t>(address.addressLo);
    }

    [[nodiscard]] std::optional<AvcReply> Resolve(std::span<const uint8_t> command) {
        if (!scripted_.empty()) {
            AvcReply reply = std::move(scripted_.front());
            scripted_.pop_front();
            return reply;
        }
        if (deviceModel_) {
            if (auto modelled = deviceModel_(command)) {
                return modelled;
            }
        }
        return defaultReply_;
    }

    void DeliverResponse(std::span<const uint8_t> command, const AvcReply& reply) {
        if (reply.kind == AvcReply::Kind::kNone || !transport_) {
            return;
        }

        std::vector<uint8_t> payload;
        if (reply.kind == AvcReply::Kind::kRaw) {
            payload = reply.raw;
        } else {
            payload.assign(command.begin(), command.end());
            if (!payload.empty()) {
                payload[0] = reply.responseCode;
            }
            for (const auto& [index, value] : reply.patches) {
                if (index < payload.size()) {
                    payload[index] = value;
                }
            }
            if (reply.truncateTo.has_value() && *reply.truncateTo < payload.size()) {
                payload.resize(*reply.truncateTo);
            }
        }

        transport_->OnFCPResponse(srcNodeId_, generation_, payload);
    }

    Async::Testing::DeferredFireWireBus& bus_;
    std::shared_ptr<Protocols::AVC::FCPTransport> transport_;
    uint16_t srcNodeId_;
    uint32_t generation_;

    std::deque<AvcReply> scripted_;
    DeviceModel deviceModel_;
    AvcReply defaultReply_{AvcReply::Accepted()};
    std::vector<Protocols::AVC::FCPFrame> commands_;
};

} // namespace ASFW::Testing
