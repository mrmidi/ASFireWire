// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RecordingFireWireBus.hpp - An IFireWireBus implementation that records every request
// (as RecordedOp and as WireTrace for golden files), validates generations, supports
// one-shot fault injection, and supports scripted response replay.
// Extracted from DICEDuplexTestSupport.hpp for cross-subsystem reuse (DICE and AV/C).

#pragma once

#include "WireTrace.hpp"

#include "ASFWDriver/Async/AsyncTypes.hpp"
#include "ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "ASFWDriver/Common/FWCommon.hpp"
#include "ASFWDriver/Common/FWTypes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ASFW::Testing {

enum class OpKind {
    Read,
    Write,
    Lock,
};

struct RecordedOp {
    OpKind kind;
    uint16_t addressHi;
    uint32_t addressLo;
    uint32_t length;
    FW::FwSpeed speed;
    uint32_t responseLength{0};
    std::vector<uint8_t> payload;
};

struct ByteView {
    const uint8_t* data;
    std::size_t size;
};

struct ExpectedOp {
    OpKind kind;
    uint32_t addressLo;
    uint32_t length;
    FW::FwSpeed speed;
};

struct ExpectedRequest {
    OpKind kind;
    uint16_t addressHi;
    uint32_t addressLo;
    uint32_t length;
    FW::FwSpeed speed;
    uint32_t responseLength;
    ByteView payload;
};

struct ResponseStep {
    OpKind kind;
    uint16_t addressHi;
    uint32_t addressLo;
    uint32_t requestLength;
    uint32_t responseLength;
    FW::FwSpeed speed;
    Async::AsyncStatus status;
    ByteView payload;
};

class RecordingFireWireBus : public Async::IFireWireBus {
public:
    RecordingFireWireBus() {
        generation_ = FW::Generation{1};
        localNodeId_ = FW::NodeId{0};
        speeds_.fill(FW::FwSpeed::S400);
    }

    ~RecordingFireWireBus() override = default;

    RecordingFireWireBus(const RecordingFireWireBus&) = delete;
    RecordingFireWireBus& operator=(const RecordingFireWireBus&) = delete;

    Async::AsyncHandle ReadBlock(FW::Generation generation,
                                 FW::NodeId nodeId,
                                 Async::FWAddress address,
                                 uint32_t length,
                                 FW::FwSpeed speed,
                                 Async::InterfaceCompletionCallback callback) override {
        (void)nodeId;
        Record(OpKind::Read, address, length, speed, 0, {});
        if (TakeDrop(OpKind::Read, address)) {
            return NextHandle();
        }
        if (const auto failure = Failure(OpKind::Read, generation, address)) {
            trace_.Read(address.addressHi, address.addressLo, length, speed, *failure);
            callback(*failure, {});
            return NextHandle();
        }

        if (HasScript()) {
            ExpectScriptedRequest(OpKind::Read, address, length, speed, 0, {});
            const auto response = TakeScriptedResponse(OpKind::Read, address, length, 0, speed);
            trace_.Read(address.addressHi, address.addressLo, length, speed, response.status);
            callback(response.status,
                     std::span<const uint8_t>(response.payload.data(), response.payload.size()));
            return NextHandle();
        }

        const auto payload = ReadPayload(address, length);
        trace_.Read(address.addressHi, address.addressLo, length, speed, Async::AsyncStatus::kSuccess);
        callback(Async::AsyncStatus::kSuccess, std::span<const uint8_t>(payload.data(), payload.size()));
        return NextHandle();
    }

    Async::AsyncHandle WriteBlock(FW::Generation generation,
                                  FW::NodeId nodeId,
                                  Async::FWAddress address,
                                  std::span<const uint8_t> data,
                                  FW::FwSpeed speed,
                                  Async::InterfaceCompletionCallback callback) override {
        (void)nodeId;
        std::vector<uint8_t> payload(data.begin(), data.end());
        Record(OpKind::Write, address, static_cast<uint32_t>(data.size()), speed, 0, payload);
        if (TakeDrop(OpKind::Write, address)) {
            return NextHandle();
        }
        if (const auto failure = Failure(OpKind::Write, generation, address)) {
            trace_.Write(address.addressHi, address.addressLo, data, speed, *failure);
            callback(*failure, {});
            return NextHandle();
        }

        if (HasScript()) {
            ExpectScriptedRequest(OpKind::Write,
                                  address,
                                  static_cast<uint32_t>(data.size()),
                                  speed,
                                  0,
                                  payload);
        }

        trace_.Write(address.addressHi, address.addressLo, data, speed, Async::AsyncStatus::kSuccess);
        WritePayload(address, data);
        callback(Async::AsyncStatus::kSuccess, {});
        return NextHandle();
    }

    Async::AsyncHandle Lock(FW::Generation generation,
                            FW::NodeId nodeId,
                            Async::FWAddress address,
                            FW::LockOp lockOp,
                            std::span<const uint8_t> operand,
                            uint32_t responseLength,
                            FW::FwSpeed speed,
                            Async::InterfaceCompletionCallback callback) override {
        (void)nodeId;
        (void)lockOp;
        std::vector<uint8_t> payload(operand.begin(), operand.end());
        Record(OpKind::Lock, address, static_cast<uint32_t>(operand.size()), speed, responseLength, payload);
        if (TakeDrop(OpKind::Lock, address)) {
            return NextHandle();
        }
        const bool compareSwap64 = operand.size() == 16 && responseLength == 8;
        const uint64_t expected = compareSwap64 ? FW::ReadBE64(operand.data())
                                                : (operand.size() >= 4 ? FW::ReadBE32(operand.data()) : 0);
        const uint64_t desired = compareSwap64 ? FW::ReadBE64(operand.data() + 8)
                                               : (operand.size() >= 8 ? FW::ReadBE32(operand.data() + 4) : 0);
        if (const auto failure = Failure(OpKind::Lock, generation, address)) {
            trace_.CompareSwap(address.addressHi, address.addressLo, expected, desired,
                               std::nullopt, speed, *failure, compareSwap64);
            callback(*failure, {});
            return NextHandle();
        }

        if (HasScript()) {
            ExpectScriptedRequest(OpKind::Lock,
                                  address,
                                  static_cast<uint32_t>(operand.size()),
                                  speed,
                                  responseLength,
                                  payload);
            const auto previous = ApplyLock(address, compareSwap64, expected, desired);
            const auto response = TakeScriptedResponse(
                OpKind::Lock, address, static_cast<uint32_t>(operand.size()), responseLength, speed);
            trace_.CompareSwap(address.addressHi, address.addressLo, expected, desired, previous,
                               speed, response.status, compareSwap64);
            callback(response.status,
                     std::span<const uint8_t>(response.payload.data(), response.payload.size()));
            return NextHandle();
        }

        const auto previous = ApplyLock(address, compareSwap64, expected, desired);
        std::vector<uint8_t> response(responseLength, 0);
        if (previous && responseLength == 8) {
            FW::WriteBE64(response.data(), *previous);
        } else if (previous && responseLength == 4) {
            FW::WriteBE32(response.data(), static_cast<uint32_t>(*previous));
        }
        trace_.CompareSwap(address.addressHi, address.addressLo, expected, desired, previous,
                           speed, Async::AsyncStatus::kSuccess, compareSwap64);
        callback(Async::AsyncStatus::kSuccess, std::span<const uint8_t>(response.data(), response.size()));
        return NextHandle();
    }

    bool Cancel(Async::AsyncHandle handle) override {
        (void)handle;
        return false;
    }

    FW::FwSpeed GetSpeed(FW::NodeId nodeId) const override {
        return speeds_[nodeId.value];
    }

    bool RecordVerifiedSpeed(FW::Generation generation, FW::NodeId nodeId, FW::FwSpeed speed) override {
        if (generation != generation_) {
            return false;
        }
        auto& current = speeds_[nodeId.value];
        if (static_cast<uint8_t>(speed) < static_cast<uint8_t>(current)) {
            current = speed;
        }
        return true;
    }

    void SetSpeed(FW::NodeId nodeId, FW::FwSpeed speed) {
        speeds_[nodeId.value] = speed;
    }

    uint8_t GetGapCount() const override { return gapCount_; }
    void SetGapCount(uint8_t gapCount) { gapCount_ = gapCount; }

    uint32_t HopCount(FW::NodeId nodeA, FW::NodeId nodeB) const override {
        (void)nodeA;
        (void)nodeB;
        return 1;
    }

    FW::Generation GetGeneration() const override {
        return generation_;
    }

    void SetGeneration(FW::Generation generation) {
        generation_ = generation;
    }

    FW::NodeId GetLocalNodeID() const override {
        return localNodeId_;
    }

    void SetLocalNodeID(FW::NodeId nodeId) {
        localNodeId_ = nodeId;
    }

    void ClearOperations() {
        operations_.clear();
    }

    const std::vector<RecordedOp>& Operations() const {
        return operations_;
    }

    [[nodiscard]] WireTrace& Trace() noexcept { return trace_; }
    [[nodiscard]] const WireTrace& Trace() const noexcept { return trace_; }

    virtual void BusReset() {
        generation_ = FW::Generation{generation_.value + 1};
        char line[48];
        std::snprintf(line, sizeof(line), "# bus-reset gen=%u", generation_.value);
        trace_.Add(line);
        OnBusReset();
    }

    void FailNext(OpKind kind, uint32_t addressLo, Async::AsyncStatus status) {
        faults_.push_back(Fault{kind, addressLo, status});
    }

    void DropNext(OpKind kind, uint32_t addressLo) {
        drops_.push_back(Fault{kind, addressLo, Async::AsyncStatus::kSuccess});
    }

    void SetScript(std::span<const ExpectedRequest> requests,
                   std::span<const ResponseStep> responses) {
        scriptedRequests_ = requests;
        scriptedResponses_ = responses;
        scriptedRequestIndex_ = 0;
        scriptedResponseIndex_ = 0;
    }

    void ClearScript() {
        scriptedRequests_ = {};
        scriptedResponses_ = {};
        scriptedRequestIndex_ = 0;
        scriptedResponseIndex_ = 0;
    }

    [[nodiscard]] bool ScriptConsumed() const {
        return !HasScript() ||
               (scriptedRequestIndex_ == scriptedRequests_.size() &&
                scriptedResponseIndex_ == scriptedResponses_.size());
    }

    // Callbacks for dynamic responders without subclassing:
    void SetReadResponder(std::function<std::optional<std::vector<uint8_t>>(Async::FWAddress, uint32_t)> fn) {
        readResponder_ = std::move(fn);
    }
    void SetWriteResponder(std::function<void(Async::FWAddress, std::span<const uint8_t>)> fn) {
        writeResponder_ = std::move(fn);
    }
    void SetLockResponder(std::function<std::optional<uint64_t>(Async::FWAddress, bool, uint64_t, uint64_t)> fn) {
        lockResponder_ = std::move(fn);
    }
    void SetBusResetResponder(std::function<void()> fn) {
        busResetResponder_ = std::move(fn);
    }

protected:
    virtual std::vector<uint8_t> ReadPayload(Async::FWAddress address, uint32_t length) {
        if (readResponder_) {
            if (auto res = readResponder_(address, length)) {
                return *res;
            }
        }
        return std::vector<uint8_t>(length, 0);
    }

    virtual void WritePayload(Async::FWAddress address, std::span<const uint8_t> data) {
        if (writeResponder_) {
            writeResponder_(address, data);
        }
    }

    virtual std::optional<uint64_t> ApplyLock(Async::FWAddress address, bool compareSwap64,
                                              uint64_t expected, uint64_t desired) {
        if (lockResponder_) {
            return lockResponder_(address, compareSwap64, expected, desired);
        }
        return std::nullopt;
    }

    virtual void OnBusReset() {
        if (busResetResponder_) {
            busResetResponder_();
        }
    }

    Async::AsyncHandle NextHandle() {
        return Async::AsyncHandle{nextHandle_++};
    }

    WireTrace trace_;
    FW::Generation generation_{0};
    FW::NodeId localNodeId_{0};
    std::array<FW::FwSpeed, 64> speeds_{};
    uint8_t gapCount_{63};
    uint32_t nextHandle_{1};

private:
    struct ScriptResponse {
        Async::AsyncStatus status;
        std::vector<uint8_t> payload;
    };

    struct Fault {
        OpKind kind;
        uint32_t addressLo;
        Async::AsyncStatus status;
    };

    std::optional<Async::AsyncStatus> Failure(OpKind kind, FW::Generation generation, Async::FWAddress address) {
        if (generation != generation_) {
            return Async::AsyncStatus::kStaleGeneration;
        }
        for (auto it = faults_.begin(); it != faults_.end(); ++it) {
            if (it->kind == kind && it->addressLo == address.addressLo) {
                const Async::AsyncStatus status = it->status;
                faults_.erase(it);
                return status;
            }
        }
        return std::nullopt;
    }

    bool TakeDrop(OpKind kind, Async::FWAddress address) {
        for (auto it = drops_.begin(); it != drops_.end(); ++it) {
            if (it->kind == kind && it->addressLo == address.addressLo) {
                drops_.erase(it);
                trace_.Add("# completion dropped");
                return true;
            }
        }
        return false;
    }

    void Record(OpKind kind,
                Async::FWAddress address,
                uint32_t length,
                FW::FwSpeed speed,
                uint32_t responseLength,
                std::vector<uint8_t> payload) {
        operations_.push_back(RecordedOp{
            .kind = kind,
            .addressHi = address.addressHi,
            .addressLo = address.addressLo,
            .length = length,
            .speed = speed,
            .responseLength = responseLength,
            .payload = std::move(payload),
        });
    }

    [[nodiscard]] bool HasScript() const {
        return !scriptedRequests_.empty() || !scriptedResponses_.empty();
    }

    void ExpectScriptedRequest(OpKind kind,
                               Async::FWAddress address,
                               uint32_t length,
                               FW::FwSpeed speed,
                               uint32_t responseLength,
                               std::span<const uint8_t> payload) {
        if (scriptedRequestIndex_ >= scriptedRequests_.size()) {
            ADD_FAILURE() << "unexpected scripted request past end of fixture";
            return;
        }
        const auto& expected = scriptedRequests_[scriptedRequestIndex_++];
        EXPECT_EQ(expected.kind, kind) << "script request " << (scriptedRequestIndex_ - 1);
        EXPECT_EQ(expected.addressHi, address.addressHi) << "script request " << (scriptedRequestIndex_ - 1);
        EXPECT_EQ(expected.addressLo, address.addressLo) << "script request " << (scriptedRequestIndex_ - 1);
        EXPECT_EQ(expected.length, length) << "script request " << (scriptedRequestIndex_ - 1);
        EXPECT_EQ(expected.speed, speed) << "script request " << (scriptedRequestIndex_ - 1);
        EXPECT_EQ(expected.responseLength, responseLength) << "script request " << (scriptedRequestIndex_ - 1);
        EXPECT_EQ(expected.payload.size, payload.size()) << "script request " << (scriptedRequestIndex_ - 1);
        if (expected.payload.size == payload.size() && expected.payload.size > 0) {
            EXPECT_TRUE(std::equal(expected.payload.data,
                                   expected.payload.data + expected.payload.size,
                                   payload.begin()))
                << "script request " << (scriptedRequestIndex_ - 1);
        }
    }

    ScriptResponse TakeScriptedResponse(OpKind kind,
                                        Async::FWAddress address,
                                        uint32_t requestLength,
                                        uint32_t responseLength,
                                        FW::FwSpeed speed) {
        if (scriptedResponseIndex_ >= scriptedResponses_.size()) {
            ADD_FAILURE() << "unexpected scripted response past end of fixture";
            return ScriptResponse{.status = Async::AsyncStatus::kTimeout, .payload = {}};
        }
        const auto& expected = scriptedResponses_[scriptedResponseIndex_++];
        EXPECT_EQ(expected.kind, kind) << "script response " << (scriptedResponseIndex_ - 1);
        EXPECT_EQ(expected.addressHi, address.addressHi) << "script response " << (scriptedResponseIndex_ - 1);
        EXPECT_EQ(expected.addressLo, address.addressLo) << "script response " << (scriptedResponseIndex_ - 1);
        EXPECT_EQ(expected.requestLength, requestLength) << "script response " << (scriptedResponseIndex_ - 1);
        EXPECT_EQ(expected.responseLength, responseLength == 0 ? expected.responseLength : responseLength)
            << "script response " << (scriptedResponseIndex_ - 1);
        EXPECT_EQ(expected.speed, speed) << "script response " << (scriptedResponseIndex_ - 1);

        std::vector<uint8_t> payload;
        if (expected.payload.size > 0) {
            payload.assign(expected.payload.data, expected.payload.data + expected.payload.size);
        }
        return ScriptResponse{
            .status = expected.status,
            .payload = std::move(payload),
        };
    }

    std::vector<Fault> faults_;
    std::vector<Fault> drops_;
    std::vector<RecordedOp> operations_;

    std::span<const ExpectedRequest> scriptedRequests_{};
    std::span<const ResponseStep> scriptedResponses_{};
    std::size_t scriptedRequestIndex_{0};
    std::size_t scriptedResponseIndex_{0};

    std::function<std::optional<std::vector<uint8_t>>(Async::FWAddress, uint32_t)> readResponder_;
    std::function<void(Async::FWAddress, std::span<const uint8_t>)> writeResponder_;
    std::function<std::optional<uint64_t>(Async::FWAddress, bool, uint64_t, uint64_t)> lockResponder_;
    std::function<void()> busResetResponder_;
};

} // namespace ASFW::Testing
