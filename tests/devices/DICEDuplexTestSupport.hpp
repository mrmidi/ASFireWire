// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DICEDuplexTestSupport.hpp - Shared test mocks and fixtures for DICE duplex and IRM tests

#pragma once

#include <gtest/gtest.h>

#include "FakeTimerScheduler.hpp"
#include "Testing/HostDriverKitStubs.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Common/WireFormat.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Audio/Protocols/DICE/Core/DICENotificationMailbox.hpp"
#include "Audio/Protocols/DICE/Core/DICETransaction.hpp"
#include "Audio/Protocols/DICE/Core/DiceDeviceIo.hpp"
#include "Audio/Protocols/DICE/Core/DiceFamilyDriver.hpp"
#include "FakeDiceWaitClock.hpp"
#include "Protocols/Ports/ProtocolRegisterIO.hpp"
#include "SimulatedDiceDevice.hpp"
#include "WireTrace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::Testing::DICE {

using ::ASFW::Async::AsyncHandle;
using ::ASFW::Async::AsyncStatus;
using ::ASFW::Async::FWAddress;
using ::ASFW::Async::IFireWireBus;
using ::ASFW::Audio::AudioDuplexChannels;
using ::ASFW::Audio::DICE::ClockSource;
using ::ASFW::Audio::DICE::DICETransaction;
using ::ASFW::Audio::DICE::DICEBringupPolicy;
using ::ASFW::Audio::DuplexConfirmResult;
using ::ASFW::Audio::DuplexPrepareResult;
using ::ASFW::Audio::DuplexStageResult;
using ::ASFW::Audio::DICE::GeneralSections;
using ::ASFW::Audio::DICE::kOwnerNoOwner;
using ::ASFW::Audio::DICE::MakeDICEAddress;
using ::ASFW::Audio::DuplexRestartPhase;
using ::ASFW::Audio::DuplexRestartReason;
namespace NotificationMailbox = ::ASFW::Audio::DICE::NotificationMailbox;
using ::ASFW::Audio::DICE::Section;
using ::ASFW::Audio::DICE::DiceDeviceIo;
using ::ASFW::Audio::DICE::DiceFamilyDriver;
using ::ASFW::Audio::DICE::DiceClockConfiguration;
using ::ASFW::FW::FwSpeed;
using ::ASFW::FW::Generation;
using ::ASFW::FW::LockOp;
using ::ASFW::FW::NodeId;
using ::ASFW::Protocols::Ports::ProtocolRegisterIO;
namespace ClockRateIndex = ::ASFW::Audio::DICE::ClockRateIndex;
namespace ClockSelectBits = ::ASFW::Audio::DICE::ClockSelect;
namespace GlobalOffset = ::ASFW::Audio::DICE::GlobalOffset;
namespace NotifyBits = ::ASFW::Audio::DICE::Notify;
namespace RxOffset = ::ASFW::Audio::DICE::RxOffset;
namespace StatusBits = ::ASFW::Audio::DICE::StatusBits;
namespace TxOffset = ::ASFW::Audio::DICE::TxOffset;

struct RouteState {
    ::ASFW::Discovery::DeviceRegistry registry;
    ::ASFW::Discovery::DeviceRouteToken route{};

    RouteState() {
        ::ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = 0xD1CE000000000002ULL;
        rom.gen = Generation{1};
        rom.nodeId = 0x02;
        (void)registry.UpsertFromROM(rom, ::ASFW::Discovery::LinkPolicy{});
        route = *registry.CurrentRoute(rom.bib.guid);
    }
};

constexpr uint32_t kGeneralSectionBytes = 40;
constexpr uint32_t kGlobalBytes = 380;
constexpr uint32_t kTxSectionOffset = 0x01A4;
constexpr uint32_t kRxSectionOffset = 0x03DC;
constexpr uint32_t kTxEntryQuadlets = 70;
constexpr uint32_t kRxEntryQuadlets = 70;
constexpr uint32_t kClockSelect48kInternal =
    (ClockRateIndex::k48000 << ClockSelectBits::kRateShift) |
    static_cast<uint32_t>(ClockSource::Internal);
constexpr uint32_t kLocked48kStatus =
    StatusBits::kSourceLocked |
    (ClockRateIndex::k48000 << StatusBits::kNominalRateShift);

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
    FwSpeed speed;
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
    FwSpeed speed;
};

struct ExpectedRequest {
    OpKind kind;
    uint16_t addressHi;
    uint32_t addressLo;
    uint32_t length;
    FwSpeed speed;
    uint32_t responseLength;
    ByteView payload;
};

struct ResponseStep {
    OpKind kind;
    uint16_t addressHi;
    uint32_t addressLo;
    uint32_t requestLength;
    uint32_t responseLength;
    FwSpeed speed;
    AsyncStatus status;
    ByteView payload;
};

#include "ReferencePhase0ParityFixture.inc"

inline void PutBe32(uint8_t* dst, uint32_t value) {
    ::ASFW::FW::WriteBE32(dst, value);
}

inline void PutBe64(uint8_t* dst, uint64_t value) {
    ::ASFW::FW::WriteBE64(dst, value);
}

inline std::array<uint8_t, kGeneralSectionBytes> MakeGeneralSectionsWire() {
    std::array<uint8_t, kGeneralSectionBytes> bytes{};

    PutBe32(bytes.data() + 0x00, 0x0000000A);  // global offset 0x28
    PutBe32(bytes.data() + 0x04, 0x0000005F);  // global size 380
    PutBe32(bytes.data() + 0x08, 0x00000069);  // tx offset 0x1a4
    PutBe32(bytes.data() + 0x0C, 0x00000046);  // tx size 280
    PutBe32(bytes.data() + 0x10, 0x000000F7);  // rx offset 0x3dc
    PutBe32(bytes.data() + 0x14, 0x00000046);  // rx size 280

    return bytes;
}

inline GeneralSections MakeGeneralSections() {
    return GeneralSections{
        .global = Section{.offset = 0x0028, .size = kGlobalBytes},
        .txStreamFormat = Section{.offset = kTxSectionOffset, .size = kTxEntryQuadlets * 4},
        .rxStreamFormat = Section{.offset = kRxSectionOffset, .size = kRxEntryQuadlets * 4},
        .extSync = Section{},
        .reserved = Section{},
    };
}

// The single-device layout the controller tests were written against: a
// Saffire Pro 24 DSP-like section table and stream shape, with no clock-source
// names. Those tests and the reference-parity scripts run on it unchanged.
// (Channel names are now stored little-endian within each quadlet, as a real
// DICE device stores them; the old fake stored them in reading order.)
inline constexpr DiceDeviceImage kLegacyTestDeviceImage{
    .key = "legacy-test-device",
    .reportedModel = "DICEDuplexTestSupport legacy layout",
    .source = "DICEDuplexTestSupport.hpp",
    .guid = 0xD1CE000000000002ULL,
    .globalSection = {.offsetQuadlets = 0x0A, .sizeQuadlets = 0x5F},
    .txSection = {.offsetQuadlets = 0x69, .sizeQuadlets = 0x46},
    .rxSection = {.offsetQuadlets = 0xF7, .sizeQuadlets = 0x46},
    .extSyncSection = {},
    .hasExtension = false,
    .owner = kOwnerNoOwner,
    .notification = 0x00000010U,
    .nickname = "",
    .clockSelect = 0,
    .enable = 0,
    .status = kLocked48kStatus,
    .extStatus = 0,
    .sampleRate = 48000,
    .version = 0x01000C00,
    .clockCaps = 0x00001E06,
    .clockSourceNames = "",
    .txEntryQuadlets = kTxEntryQuadlets,
    .rxEntryQuadlets = kRxEntryQuadlets,
    .txCount = 1,
    .tx = {{{.iso = -1, .pcm = 16, .midi = 1, .speedOrSeqStart = 2, .names = "IP 1"}}},
    .rxCount = 1,
    .rx = {{{.iso = -1, .pcm = 8, .midi = 1, .speedOrSeqStart = 0, .names = "Mon 1"}}},
};

// IFireWireBus over one SimulatedDiceDevice. Records every request (as
// RecordedOp for the existing parity checks, and as a WireTrace for golden
// files), enforces the bus generation, and can inject one-shot faults.
// Scripted mode replays recorded responses instead of asking the device;
// writes and locks still reach the device so its state stays coherent.
class RecordingFireWireBus final : public IFireWireBus {
public:
    explicit RecordingFireWireBus(const DiceDeviceImage& image = kLegacyTestDeviceImage,
                                  SimulatedDiceOptions options = {})
        : device_(image, options), defaultClockResponse_(options.clockResponse) {
        generation_ = Generation{1};
        localNodeId_ = NodeId{0};
        speeds_[0x02] = FwSpeed::S400;
        device_.SetNotifySink([](uint32_t bits) { NotificationMailbox::Publish(bits); });
        device_.SetTraceSink([this](std::string_view line) { trace_.Add(line); });
    }

    RecordingFireWireBus(const RecordingFireWireBus&) = delete;
    RecordingFireWireBus& operator=(const RecordingFireWireBus&) = delete;

    AsyncHandle ReadBlock(Generation generation,
                          NodeId nodeId,
                          FWAddress address,
                          uint32_t length,
                          FwSpeed speed,
                          ::ASFW::Async::InterfaceCompletionCallback callback) override {
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
        trace_.Read(address.addressHi, address.addressLo, length, speed, AsyncStatus::kSuccess);
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(payload.data(), payload.size()));
        return NextHandle();
    }

    AsyncHandle WriteBlock(Generation generation,
                           NodeId nodeId,
                           FWAddress address,
                           std::span<const uint8_t> data,
                           FwSpeed speed,
                           ::ASFW::Async::InterfaceCompletionCallback callback) override {
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

        trace_.Write(address.addressHi, address.addressLo, data, speed, AsyncStatus::kSuccess);
        if (address.addressHi == kDiceBaseAddressHi) {
            (void)device_.Write(address.addressLo, data);
        }
        if (clockSelectWriteHandler_ && IsClockSelect(address)) {
            clockSelectWriteHandler_();
        }
        callback(AsyncStatus::kSuccess, {});
        return NextHandle();
    }

    AsyncHandle Lock(Generation generation,
                     NodeId nodeId,
                     FWAddress address,
                     LockOp lockOp,
                     std::span<const uint8_t> operand,
                     uint32_t responseLength,
                     FwSpeed speed,
                     ::ASFW::Async::InterfaceCompletionCallback callback) override {
        (void)nodeId;
        (void)lockOp;
        std::vector<uint8_t> payload(operand.begin(), operand.end());
        Record(OpKind::Lock, address, static_cast<uint32_t>(operand.size()), speed, responseLength, payload);
        if (TakeDrop(OpKind::Lock, address)) {
            return NextHandle();
        }
        const bool compareSwap64 = operand.size() == 16 && responseLength == 8;
        const uint64_t expected = compareSwap64 ? ::ASFW::FW::ReadBE64(operand.data()) : 0;
        const uint64_t desired = compareSwap64 ? ::ASFW::FW::ReadBE64(operand.data() + 8) : 0;
        if (const auto failure = Failure(OpKind::Lock, generation, address)) {
            trace_.CompareSwap(address.addressHi, address.addressLo, expected, desired,
                               std::nullopt, speed, *failure);
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
                               speed, response.status);
            callback(response.status,
                     std::span<const uint8_t>(response.payload.data(), response.payload.size()));
            return NextHandle();
        }

        const auto previous = ApplyLock(address, compareSwap64, expected, desired);
        std::vector<uint8_t> response(responseLength, 0);
        if (previous && responseLength == 8) {
            PutBe64(response.data(), *previous);
        }
        trace_.CompareSwap(address.addressHi, address.addressLo, expected, desired, previous,
                           speed, AsyncStatus::kSuccess);
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(response.data(), response.size()));
        return NextHandle();
    }

    bool Cancel(AsyncHandle handle) override {
        (void)handle;
        return false;
    }

    FwSpeed GetSpeed(NodeId nodeId) const override {
        return speeds_[nodeId.value];
    }

    bool RecordVerifiedSpeed(Generation generation, NodeId nodeId, FwSpeed speed) override {
        if (generation != generation_) {
            return false;
        }
        auto& current = speeds_[nodeId.value];
        if (static_cast<uint8_t>(speed) < static_cast<uint8_t>(current)) {
            current = speed;
        }
        return true;
    }

    void SetSpeed(NodeId nodeId, FwSpeed speed) {
        speeds_[nodeId.value] = speed;
    }

    [[nodiscard]] uint32_t TxSpeed() const {
        return device_.TxSpeed(0);
    }

    uint8_t GetGapCount() const override { return gapCount_; }

    void SetGapCount(uint8_t gapCount) { gapCount_ = gapCount; }

    uint32_t HopCount(NodeId nodeA, NodeId nodeB) const override {
        (void)nodeA;
        (void)nodeB;
        return 1;
    }

    Generation GetGeneration() const override {
        return generation_;
    }

    NodeId GetLocalNodeID() const override {
        return localNodeId_;
    }

    void ClearOperations() {
        operations_.clear();
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

    const std::vector<RecordedOp>& Operations() const {
        return operations_;
    }

    uint64_t Owner() const {
        return device_.Owner();
    }

    uint32_t Enable() const {
        return device_.Enable();
    }

    // Replace the device's own CLOCK_SELECT response with `handler`, which runs
    // after the write lands and before its completion is delivered.
    void SetClockSelectWriteHandler(std::function<void()> handler) {
        clockSelectWriteHandler_ = std::move(handler);
        auto options = device_.GetOptions();
        options.clockResponse = clockSelectWriteHandler_ ? DiceClockResponse::kNeverAccept
                                                         : defaultClockResponse_;
        device_.SetOptions(options);
    }

    void SetGlobalClockState(uint32_t status, uint32_t sampleRate,
                             uint32_t notification = 0) {
        device_.SetGlobalQuad(GlobalOffset::kStatus, status);
        device_.SetGlobalQuad(GlobalOffset::kSampleRate, sampleRate);
        device_.SetGlobalQuad(GlobalOffset::kNotification, notification);
    }

    void SetGeneration(Generation generation) {
        generation_ = generation;
    }

    void SetLocalNodeID(NodeId nodeId) {
        localNodeId_ = nodeId;
    }

    void SetStreamIsoChannels(uint32_t txIso, uint32_t rxIso) {
        device_.SetTxIso(0, txIso);
        device_.SetRxIso(0, rxIso);
    }

    void PublishClockAccepted(uint32_t bits = NotifyBits::kClockAccepted) {
        device_.SetAchievedClock(ClockRateIndex::k48000, true);
        device_.RaiseNotification(bits);
    }

    void LatchClockAccepted(uint32_t bits = NotifyBits::kClockAccepted) {
        device_.SetAchievedClock(ClockRateIndex::k48000, true);
        device_.SetNotificationRegister(bits);
    }

    // ---- simulator access, faults and trace --------------------------------

    [[nodiscard]] SimulatedDiceDevice& Device() noexcept { return device_; }
    [[nodiscard]] const SimulatedDiceDevice& Device() const noexcept { return device_; }
    [[nodiscard]] ::ASFW::Testing::WireTrace& Trace() noexcept { return trace_; }

    // A bus reset: new generation, and the device's own reset behaviour.
    void BusReset() {
        generation_ = Generation{generation_.value + 1};
        char line[48];
        std::snprintf(line, sizeof(line), "# bus-reset gen=%u", generation_.value);
        trace_.Add(line);
        device_.BusReset();
    }

    // Fail the next request of `kind` at `addressLo` with `status`, once.
    void FailNext(OpKind kind, uint32_t addressLo, AsyncStatus status) {
        faults_.push_back(Fault{kind, addressLo, status});
    }

    // Swallow the next request of `kind` at `addressLo`: it is recorded, but its
    // completion never arrives (a wedged or unreachable completion path).
    void DropNext(OpKind kind, uint32_t addressLo) {
        drops_.push_back(Fault{kind, addressLo, AsyncStatus::kSuccess});
    }

private:
    struct ScriptResponse {
        AsyncStatus status;
        std::vector<uint8_t> payload;
    };

    struct Fault {
        OpKind kind;
        uint32_t addressLo;
        AsyncStatus status;
    };

    AsyncHandle NextHandle() {
        return AsyncHandle{nextHandle_++};
    }

    [[nodiscard]] bool IsClockSelect(FWAddress address) const {
        return address.addressHi == kDiceBaseAddressHi &&
               address.addressLo == kDiceBaseAddressLo + device_.GlobalBase() + GlobalOffset::kClockSelect;
    }

    // Stale generation first (a real bus rejects it before the device sees it),
    // then any injected one-shot fault.
    std::optional<AsyncStatus> Failure(OpKind kind, Generation generation, FWAddress address) {
        if (generation != generation_) {
            return AsyncStatus::kStaleGeneration;
        }
        for (auto it = faults_.begin(); it != faults_.end(); ++it) {
            if (it->kind == kind && it->addressLo == address.addressLo) {
                const AsyncStatus status = it->status;
                faults_.erase(it);
                return status;
            }
        }
        return std::nullopt;
    }

    bool TakeDrop(OpKind kind, FWAddress address) {
        for (auto it = drops_.begin(); it != drops_.end(); ++it) {
            if (it->kind == kind && it->addressLo == address.addressLo) {
                drops_.erase(it);
                trace_.Add("# completion dropped");
                return true;
            }
        }
        return false;
    }

    std::optional<uint64_t> ApplyLock(FWAddress address, bool compareSwap64,
                                      uint64_t expected, uint64_t desired) {
        if (!compareSwap64 || address.addressHi != kDiceBaseAddressHi) {
            return std::nullopt;
        }
        return device_.CompareSwap64(address.addressLo, expected, desired);
    }

    void Record(OpKind kind,
                FWAddress address,
                uint32_t length,
                FwSpeed speed,
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
                               FWAddress address,
                               uint32_t length,
                               FwSpeed speed,
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
                                        FWAddress address,
                                        uint32_t requestLength,
                                        uint32_t responseLength,
                                        FwSpeed speed) {
        if (scriptedResponseIndex_ >= scriptedResponses_.size()) {
            ADD_FAILURE() << "unexpected scripted response past end of fixture";
            return ScriptResponse{.status = AsyncStatus::kTimeout, .payload = {}};
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

    // Addresses outside the modelled DICE space read as zeros, as they did in
    // the legacy fake.
    std::vector<uint8_t> ReadPayload(FWAddress address, uint32_t length) const {
        if (address.addressHi == kDiceBaseAddressHi) {
            if (auto bytes = device_.Read(address.addressLo, length)) {
                return *bytes;
            }
        }
        return std::vector<uint8_t>(length, 0);
    }

    SimulatedDiceDevice device_;
    DiceClockResponse defaultClockResponse_;
    ::ASFW::Testing::WireTrace trace_;
    std::vector<Fault> faults_;
    std::vector<Fault> drops_;
    std::vector<RecordedOp> operations_;
    Generation generation_{0};
    NodeId localNodeId_{0};
    std::array<FwSpeed, 64> speeds_{[] {
        std::array<FwSpeed, 64> speeds{};
        speeds.fill(FwSpeed::S100);
        return speeds;
    }()};
    uint32_t nextHandle_{1};
    uint8_t gapCount_{63};

    std::function<void()> clockSelectWriteHandler_;
    std::span<const ExpectedRequest> scriptedRequests_{};
    std::span<const ResponseStep> scriptedResponses_{};
    std::size_t scriptedRequestIndex_{0};
    std::size_t scriptedResponseIndex_{0};
};

struct HostClockResetGuard {
    ~HostClockResetGuard() {
        ::ASFW::Testing::ResetHostMonotonicClockForTesting();
    }
};

// Callback-style view of DiceFamilyDriver. The bring-up tests were written
// against the async DICEDuplexBringupController; running the same test bodies
// against the linear driver is the equivalence check for stage S1. Each call
// completes before it returns, so the callback fires synchronously.
class CallbackDriver {
public:
    explicit CallbackDriver(DiceFamilyDriver& driver) noexcept : driver_(driver) {}

    // Reference-parity window: fixed 48 kHz internal clock, no caps refresh.
    void PrepareDuplex48k(const AudioDuplexChannels& channels, std::function<void(IOReturn)> callback) {
        const auto result = driver_.Prepare(channels, k48k, /*refreshRuntimeCaps=*/false);
        callback(result ? kIOReturnSuccess : result.error());
    }
    void PrepareDuplex(const AudioDuplexChannels& channels, const DiceClockConfiguration& clock,
                       std::function<void(IOReturn, DuplexPrepareResult)> callback) {
        Deliver(driver_.Prepare(channels, clock, /*refreshRuntimeCaps=*/true), callback);
    }
    void ProgramRxForDuplex48k(std::function<void(IOReturn)> callback) {
        const auto result = driver_.ProgramRx();
        callback(result ? kIOReturnSuccess : result.error());
    }
    void ProgramRx(std::function<void(IOReturn, DuplexStageResult)> callback) {
        Deliver(driver_.ProgramRx(), callback);
    }
    void ProgramTxAndEnableDuplex48k(std::function<void(IOReturn)> callback) {
        const auto result = driver_.ProgramTxAndEnable();
        callback(result ? kIOReturnSuccess : result.error());
    }
    void ProgramTxAndEnableDuplex(std::function<void(IOReturn, DuplexStageResult)> callback) {
        Deliver(driver_.ProgramTxAndEnable(), callback);
    }
    void ConfirmDuplex48kStart(std::function<void(IOReturn)> callback) {
        const auto result = driver_.Confirm();
        callback(result ? kIOReturnSuccess : result.error());
    }
    void ConfirmDuplexStart(std::function<void(IOReturn, DuplexConfirmResult)> callback) {
        Deliver(driver_.Confirm(), callback);
    }
    [[nodiscard]] IOReturn StopDuplex() { return driver_.Stop(); }
    void ReleaseOwner(std::function<void(IOReturn)> callback) { callback(driver_.ReleaseOwner()); }

    [[nodiscard]] bool IsPrepared() const noexcept { return driver_.IsPrepared(); }
    [[nodiscard]] bool IsArmed() const noexcept { return driver_.IsArmed(); }
    [[nodiscard]] bool IsRunning() const noexcept { return driver_.IsRunning(); }
    [[nodiscard]] bool IsOwnerClaimed() const noexcept { return driver_.IsOwnerClaimed(); }

private:
    static constexpr DiceClockConfiguration k48k{
        .sampleRateHz = 48000U,
        .clockSelect = ::ASFW::Audio::DICE::kDiceClockSelect48kInternal,
    };

    template <typename T, typename Callback>
    static void Deliver(const std::expected<T, IOReturn>& result, Callback& callback) {
        callback(result ? kIOReturnSuccess : result.error(), result ? *result : T{});
    }

    DiceFamilyDriver& driver_;
};

struct DuplexRig {
    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io;
    DICETransaction tx;
    ::ASFW::Testing::FakeTimerScheduler timer;
    ::ASFW::Testing::FakeDiceWaitClock clock{timer};
    DiceDeviceIo deviceIo;
    std::atomic<bool> cancel{false};
    DiceFamilyDriver driver;
    CallbackDriver controller{driver};

    explicit DuplexRig(DICEBringupPolicy bringupPolicy = {})
        : io(bus, bus, routeState.registry, routeState.route)
        , tx(io)
        , deviceIo(io, tx, clock)
        , driver(deviceIo, bus, MakeGeneralSections(), bringupPolicy) {
        driver.SetTeardownCancelToken(&cancel);
    }
};

inline std::vector<ExpectedOp> ExpectedStopOps() {
    return {
        {OpKind::Write, 0xE0000078U, 4, FwSpeed::S400},
        {OpKind::Read,  0xE00001A8U, 4, FwSpeed::S400},
        {OpKind::Write, 0xE00001ACU, 4, FwSpeed::S400},
        {OpKind::Write, 0xE00001B8U, 4, FwSpeed::S400},
        {OpKind::Read,  0xE00003E0U, 4, FwSpeed::S400},
        {OpKind::Write, 0xE00003E4U, 4, FwSpeed::S400},
        {OpKind::Write, 0xE00003E8U, 4, FwSpeed::S400},
        {OpKind::Lock,  0xE0000028U, 16, FwSpeed::S400},
    };
}

template <typename T>
inline std::vector<T> Concat(std::span<const T> first, std::span<const T> second) {
    std::vector<T> merged;
    merged.reserve(first.size() + second.size());
    merged.insert(merged.end(), first.begin(), first.end());
    merged.insert(merged.end(), second.begin(), second.end());
    return merged;
}

inline void ExpectRequests(const std::vector<RecordedOp>& actual,
                           std::span<const ExpectedRequest> expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].kind, expected[i].kind) << "op " << i;
        EXPECT_EQ(actual[i].addressHi, expected[i].addressHi) << "op " << i;
        EXPECT_EQ(actual[i].addressLo, expected[i].addressLo) << "op " << i;
        EXPECT_EQ(actual[i].length, expected[i].length) << "op " << i;
        EXPECT_EQ(actual[i].speed, expected[i].speed) << "op " << i;
        EXPECT_EQ(actual[i].responseLength, expected[i].responseLength) << "op " << i;
        EXPECT_EQ(actual[i].payload.size(), expected[i].payload.size) << "op " << i;
        if (actual[i].payload.size() == expected[i].payload.size && expected[i].payload.size > 0) {
            EXPECT_TRUE(std::equal(actual[i].payload.begin(),
                                   actual[i].payload.end(),
                                   expected[i].payload.data))
                << "op " << i;
        }
    }
}

inline void ExpectOperations(const std::vector<RecordedOp>& actual,
                             const std::vector<ExpectedOp>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].kind, expected[i].kind) << "op " << i;
        EXPECT_EQ(actual[i].addressLo, expected[i].addressLo) << "op " << i;
        EXPECT_EQ(actual[i].length, expected[i].length) << "op " << i;
        EXPECT_EQ(actual[i].speed, expected[i].speed) << "op " << i;
    }
}

} // namespace ASFW::Testing::DICE
