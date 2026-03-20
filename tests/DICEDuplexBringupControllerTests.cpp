#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Common/WireFormat.hpp"
#include "IRM/IRMClient.hpp"
#include "Protocols/Audio/DICE/Core/DICENotificationMailbox.hpp"
#include "Protocols/Audio/DICE/Core/DICETransaction.hpp"
#include "Protocols/Audio/DICE/Core/DICEDuplexBringupController.hpp"
#include "Protocols/Ports/ProtocolRegisterIO.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Audio::AudioDuplexChannels;
using ASFW::Audio::DICE::ClockSource;
using ASFW::Audio::DICE::DICETransaction;
using ASFW::Audio::DICE::GeneralSections;
using ASFW::Audio::DICE::kOwnerNoOwner;
using ASFW::Audio::DICE::MakeDICEAddress;
namespace NotificationMailbox = ASFW::Audio::DICE::NotificationMailbox;
using ASFW::Audio::DICE::Section;
using ASFW::Audio::DICE::DICEDuplexBringupController;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::LockOp;
using ASFW::FW::NodeId;
using ASFW::IRM::IRMClient;
using ASFW::Protocols::Ports::ProtocolRegisterIO;
namespace ClockRateIndex = ASFW::Audio::DICE::ClockRateIndex;
namespace ClockSelectBits = ASFW::Audio::DICE::ClockSelect;
namespace GlobalOffset = ASFW::Audio::DICE::GlobalOffset;
namespace NotifyBits = ASFW::Audio::DICE::Notify;
namespace RxOffset = ASFW::Audio::DICE::RxOffset;
namespace StatusBits = ASFW::Audio::DICE::StatusBits;
namespace TxOffset = ASFW::Audio::DICE::TxOffset;

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

void PutBe32(uint8_t* dst, uint32_t value) {
    ASFW::FW::WriteBE32(dst, value);
}

void PutBe64(uint8_t* dst, uint64_t value) {
    ASFW::FW::WriteBE64(dst, value);
}

std::array<uint8_t, kGeneralSectionBytes> MakeGeneralSectionsWire() {
    std::array<uint8_t, kGeneralSectionBytes> bytes{};

    PutBe32(bytes.data() + 0x00, 0x0000000A);  // global offset 0x28
    PutBe32(bytes.data() + 0x04, 0x0000005F);  // global size 380
    PutBe32(bytes.data() + 0x08, 0x00000069);  // tx offset 0x1a4
    PutBe32(bytes.data() + 0x0C, 0x00000046);  // tx size 280
    PutBe32(bytes.data() + 0x10, 0x000000F7);  // rx offset 0x3dc
    PutBe32(bytes.data() + 0x14, 0x00000046);  // rx size 280

    return bytes;
}

GeneralSections MakeGeneralSections() {
    return GeneralSections{
        .global = Section{.offset = 0x0028, .size = kGlobalBytes},
        .txStreamFormat = Section{.offset = kTxSectionOffset, .size = kTxEntryQuadlets * 4},
        .rxStreamFormat = Section{.offset = kRxSectionOffset, .size = kRxEntryQuadlets * 4},
        .extSync = Section{},
        .reserved = Section{},
    };
}

class RecordingFireWireBus final : public IFireWireBus {
public:
    RecordingFireWireBus() {
        generation_ = Generation{1};
        localNodeId_ = NodeId{0};
        speeds_[0x02] = FwSpeed::S400;
        owner_ = kOwnerNoOwner;
        clockSelect_ = 0;
        enable_ = 0;
        notification_ = 0x00000010U;
        status_ = kLocked48kStatus;
        extStatus_ = 0;
        sampleRate_ = 48000;
        version_ = 0x01000C00;
        clockCaps_ = 0x00001E06;
        txNum_ = 1;
        txSize_ = kTxEntryQuadlets;
        txIso_ = 0xFFFFFFFFU;
        txAudio_ = 16;
        txMidi_ = 1;
        txSpeed_ = 2;
        rxNum_ = 1;
        rxSize_ = kRxEntryQuadlets;
        rxIso_ = 0xFFFFFFFFU;
        rxSeq_ = 0;
        rxAudio_ = 8;
        rxMidi_ = 1;
        bandwidthAvailable_ = 4019;
        channelsAvailable31_0_ = 0x3FFFFFFFU;
        channelsAvailable63_32_ = 0xFFFFFFFFU;

        txNames_.fill(0);
        rxNames_.fill(0);
        const char txName[] = "IP 1";
        const char rxName[] = "Mon 1";
        std::copy(txName, txName + sizeof(txName), txNames_.begin());
        std::copy(rxName, rxName + sizeof(rxName), rxNames_.begin());
    }

    AsyncHandle ReadBlock(Generation generation,
                          NodeId nodeId,
                          FWAddress address,
                          uint32_t length,
                          FwSpeed speed,
                          ASFW::Async::InterfaceCompletionCallback callback) override {
        Record(OpKind::Read, address, length, speed, 0, {});
        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, {});
            return NextHandle();
        }

        if (HasScript()) {
            ExpectScriptedRequest(OpKind::Read, address, length, speed, 0, {});
            const auto response = TakeScriptedResponse(OpKind::Read, address, length, 0, speed);
            callback(response.status,
                     std::span<const uint8_t>(response.payload.data(), response.payload.size()));
            return NextHandle();
        }

        const auto payload = ReadPayload(address, length);
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(payload.data(), payload.size()));
        return NextHandle();
    }

    AsyncHandle WriteBlock(Generation generation,
                           NodeId nodeId,
                           FWAddress address,
                           std::span<const uint8_t> data,
                           FwSpeed speed,
                           ASFW::Async::InterfaceCompletionCallback callback) override {
        std::vector<uint8_t> payload(data.begin(), data.end());
        Record(OpKind::Write, address, static_cast<uint32_t>(data.size()), speed, 0, payload);
        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, {});
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

        ApplyWrite(address, data);
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
                     ASFW::Async::InterfaceCompletionCallback callback) override {
        std::vector<uint8_t> payload(operand.begin(), operand.end());
        Record(OpKind::Lock, address, static_cast<uint32_t>(operand.size()), speed, responseLength, payload);
        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, {});
            return NextHandle();
        }

        if (HasScript()) {
            ExpectScriptedRequest(OpKind::Lock,
                                  address,
                                  static_cast<uint32_t>(operand.size()),
                                  speed,
                                  responseLength,
                                  payload);
            (void)ApplyLock(address, operand, responseLength);
            const auto response = TakeScriptedResponse(
                OpKind::Lock, address, static_cast<uint32_t>(operand.size()), responseLength, speed);
            callback(response.status,
                     std::span<const uint8_t>(response.payload.data(), response.payload.size()));
            return NextHandle();
        }

        const auto response = ApplyLock(address, operand, responseLength);
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(response.data(), response.size()));
        return NextHandle();
    }

    bool Cancel(AsyncHandle handle) override {
        return false;
    }

    FwSpeed GetSpeed(NodeId nodeId) const override {
        return speeds_[nodeId.value];
    }

    uint32_t HopCount(NodeId nodeA, NodeId nodeB) const override {
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
        return owner_;
    }

    uint32_t BandwidthAvailable() const {
        return bandwidthAvailable_;
    }

    uint32_t ChannelsAvailable31_0() const {
        return channelsAvailable31_0_;
    }

    uint32_t Enable() const {
        return enable_;
    }

    void SetClockSelectWriteHandler(std::function<void()> handler) {
        clockSelectWriteHandler_ = std::move(handler);
    }

    void SetGeneration(Generation generation) {
        generation_ = generation;
    }

    void SetIRMResourceState(uint32_t bandwidthAvailable,
                             uint32_t channelsAvailable31_0,
                             uint32_t channelsAvailable63_32) {
        bandwidthAvailable_ = bandwidthAvailable;
        channelsAvailable31_0_ = channelsAvailable31_0;
        channelsAvailable63_32_ = channelsAvailable63_32;
    }

    void PublishClockAccepted(uint32_t bits = NotifyBits::kClockAccepted) {
        ApplyClockAcceptedState(bits, true);
    }

    void LatchClockAccepted(uint32_t bits = NotifyBits::kClockAccepted) {
        ApplyClockAcceptedState(bits, false);
    }

private:
    struct ScriptResponse {
        AsyncStatus status;
        std::vector<uint8_t> payload;
    };

    AsyncHandle NextHandle() {
        return AsyncHandle{nextHandle_++};
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

    std::vector<uint8_t> QuadletPayload(uint32_t value) const {
        std::vector<uint8_t> bytes(4);
        PutBe32(bytes.data(), value);
        return bytes;
    }

    std::vector<uint8_t> ReadPayload(FWAddress address, uint32_t length) const {
        if (address.addressHi == 0xFFFF && address.addressLo == 0xE0000000U &&
            length == kGeneralSectionBytes) {
            const auto bytes = MakeGeneralSectionsWire();
            return std::vector<uint8_t>(bytes.begin(), bytes.end());
        }

        if (address.addressHi == 0xFFFF && address.addressLo == 0xE0000028U) {
            auto bytes = BuildGlobalBlock();
            bytes.resize(length);
            return bytes;
        }

        if (address.addressHi == 0xFFFF && address.addressLo == 0xE00001BCU && length == 256) {
            return std::vector<uint8_t>(txNames_.begin(), txNames_.end());
        }
        if (address.addressHi == 0xFFFF && address.addressLo == 0xE00003F4U && length == 256) {
            return std::vector<uint8_t>(rxNames_.begin(), rxNames_.end());
        }

        if (address.addressHi == 0xFFFF && address.addressLo == 0xF0000220U && length == 12) {
            std::vector<uint8_t> bytes(12);
            PutBe32(bytes.data(), bandwidthAvailable_);
            PutBe32(bytes.data() + 4, channelsAvailable31_0_);
            PutBe32(bytes.data() + 8, channelsAvailable63_32_);
            return bytes;
        }

        if (address.addressHi == 0xFFFF && length == 4) {
            switch (address.addressLo) {
            case 0xE00001A4U:
                return QuadletPayload(txNum_);
            case 0xE00001A8U:
                return QuadletPayload(txSize_);
            case 0xE00001ACU:
                return QuadletPayload(txIso_);
            case 0xE00001B0U:
                return QuadletPayload(txAudio_);
            case 0xE00001B4U:
                return QuadletPayload(txMidi_);
            case 0xE00001B8U:
                return QuadletPayload(txSpeed_);
            case 0xE00003DCU:
                return QuadletPayload(rxNum_);
            case 0xE00003E0U:
                return QuadletPayload(rxSize_);
            case 0xE00003E4U:
                return QuadletPayload(rxIso_);
            case 0xE00003E8U:
                return QuadletPayload(rxSeq_);
            case 0xE00003ECU:
                return QuadletPayload(rxAudio_);
            case 0xE00003F0U:
                return QuadletPayload(rxMidi_);
            case 0xE000007CU:
                return QuadletPayload(status_);
            case 0xE0000030U:
                return QuadletPayload(notification_);
            case 0xE0000080U:
                return QuadletPayload(extStatus_);
            case 0xF0000220U:
                return QuadletPayload(bandwidthAvailable_);
            case 0xF0000224U:
                return QuadletPayload(channelsAvailable31_0_);
            case 0xF0000228U:
                return QuadletPayload(channelsAvailable63_32_);
            default:
                break;
            }
        }

        return std::vector<uint8_t>(length, 0);
    }

    std::vector<uint8_t> BuildGlobalBlock() const {
        std::vector<uint8_t> bytes(kGlobalBytes, 0);
        PutBe64(bytes.data() + GlobalOffset::kOwnerHi, owner_);
        PutBe32(bytes.data() + GlobalOffset::kNotification, notification_);
        PutBe32(bytes.data() + GlobalOffset::kClockSelect, clockSelect_);
        PutBe32(bytes.data() + GlobalOffset::kEnable, enable_);
        PutBe32(bytes.data() + GlobalOffset::kStatus, status_);
        PutBe32(bytes.data() + GlobalOffset::kExtStatus, extStatus_);
        PutBe32(bytes.data() + GlobalOffset::kSampleRate, sampleRate_);
        PutBe32(bytes.data() + GlobalOffset::kVersion, version_);
        PutBe32(bytes.data() + GlobalOffset::kClockCaps, clockCaps_);
        return bytes;
    }

    void ApplyClockAcceptedState(uint32_t bits, bool publishMailbox) {
        notification_ = bits;
        status_ = kLocked48kStatus;
        sampleRate_ = 48000;
        if (publishMailbox) {
            NotificationMailbox::Publish(bits);
        }
    }

    void ApplyWrite(FWAddress address, std::span<const uint8_t> data) {
        if (address.addressHi != 0xFFFF || data.size() != 4) {
            return;
        }

        const uint32_t value = ASFW::FW::ReadBE32(data.data());
        switch (address.addressLo) {
        case 0xE0000074U:
            clockSelect_ = value;
            if (clockSelectWriteHandler_) {
                clockSelectWriteHandler_();
            } else {
                PublishClockAccepted();
            }
            break;
        case 0xE0000078U:
            enable_ = value;
            break;
        case 0xE00001ACU:
            txIso_ = value;
            break;
        case 0xE00001B8U:
            txSpeed_ = value;
            break;
        case 0xE00003E4U:
            rxIso_ = value;
            break;
        case 0xE00003E8U:
            rxSeq_ = value;
            break;
        default:
            break;
        }
    }

    std::vector<uint8_t> ApplyLock(FWAddress address,
                                   std::span<const uint8_t> operand,
                                   uint32_t responseLength) {
        if (address.addressHi == 0xFFFF && address.addressLo == 0xE0000028U && operand.size() == 16 &&
            responseLength == 8) {
            const uint64_t expected = ASFW::FW::ReadBE64(operand.data());
            const uint64_t desired = ASFW::FW::ReadBE64(operand.data() + 8);
            const uint64_t previous = owner_;
            if (owner_ == expected) {
                owner_ = desired;
            }
            std::vector<uint8_t> response(8);
            PutBe64(response.data(), previous);
            return response;
        }

        if (address.addressHi == 0xFFFF && operand.size() == 8 && responseLength == 4) {
            uint32_t* target = nullptr;
            switch (address.addressLo) {
            case 0xF0000220U:
                target = &bandwidthAvailable_;
                break;
            case 0xF0000224U:
                target = &channelsAvailable31_0_;
                break;
            case 0xF0000228U:
                target = &channelsAvailable63_32_;
                break;
            default:
                break;
            }

            if (target != nullptr) {
                const uint32_t expected = ASFW::FW::ReadBE32(operand.data());
                const uint32_t desired = ASFW::FW::ReadBE32(operand.data() + 4);
                const uint32_t previous = *target;
                if (*target == expected) {
                    *target = desired;
                }
                std::vector<uint8_t> response(4);
                PutBe32(response.data(), previous);
                return response;
            }
        }

        return std::vector<uint8_t>(responseLength, 0);
    }

    std::vector<RecordedOp> operations_;
    Generation generation_{0};
    NodeId localNodeId_{0};
    std::array<FwSpeed, 64> speeds_{[] {
        std::array<FwSpeed, 64> speeds{};
        speeds.fill(FwSpeed::S100);
        return speeds;
    }()};
    uint32_t nextHandle_{1};

    uint64_t owner_{0};
    uint32_t clockSelect_{0};
    uint32_t enable_{0};
    uint32_t notification_{0};
    uint32_t status_{0};
    uint32_t extStatus_{0};
    uint32_t sampleRate_{0};
    uint32_t version_{0};
    uint32_t clockCaps_{0};

    uint32_t txNum_{0};
    uint32_t txSize_{0};
    uint32_t txIso_{0};
    uint32_t txAudio_{0};
    uint32_t txMidi_{0};
    uint32_t txSpeed_{0};

    uint32_t rxNum_{0};
    uint32_t rxSize_{0};
    uint32_t rxIso_{0};
    uint32_t rxSeq_{0};
    uint32_t rxAudio_{0};
    uint32_t rxMidi_{0};

    uint32_t bandwidthAvailable_{0};
    uint32_t channelsAvailable31_0_{0};
    uint32_t channelsAvailable63_32_{0};

    std::array<uint8_t, 256> txNames_{};
    std::array<uint8_t, 256> rxNames_{};
    std::function<void()> clockSelectWriteHandler_;
    std::span<const ExpectedRequest> scriptedRequests_{};
    std::span<const ResponseStep> scriptedResponses_{};
    std::size_t scriptedRequestIndex_{0};
    std::size_t scriptedResponseIndex_{0};
};

struct HostClockResetGuard {
    ~HostClockResetGuard() {
        ASFW::Testing::ResetHostMonotonicClockForTesting();
    }
};

struct DuplexRig {
    RecordingFireWireBus bus;
    ProtocolRegisterIO io;
    DICETransaction tx;
    IRMClient irm;
    DICEDuplexBringupController controller;

    DuplexRig()
        : io(bus, bus, 0x02)
        , tx(io)
        , irm(bus)
        , controller(tx, io, bus, nullptr, MakeGeneralSections()) {
        irm.SetIRMNode(0x03, Generation{1});
    }
};

std::vector<ExpectedOp> ExpectedStopOps() {
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
std::vector<T> Concat(std::span<const T> first, std::span<const T> second) {
    std::vector<T> merged;
    merged.reserve(first.size() + second.size());
    merged.insert(merged.end(), first.begin(), first.end());
    merged.insert(merged.end(), second.begin(), second.end());
    return merged;
}

void ExpectRequests(const std::vector<RecordedOp>& actual,
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

void ExpectOperations(const std::vector<RecordedOp>& actual,
                      const std::vector<ExpectedOp>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].kind, expected[i].kind) << "op " << i;
        EXPECT_EQ(actual[i].addressLo, expected[i].addressLo) << "op " << i;
        EXPECT_EQ(actual[i].length, expected[i].length) << "op " << i;
        EXPECT_EQ(actual[i].speed, expected[i].speed) << "op " << i;
    }
}

} // namespace

TEST(DICEDuplexBringupControllerTests, NotificationMailboxMatchesReferenceAndLegacyOffsets) {
    EXPECT_TRUE(NotificationMailbox::MatchesDestOffset(NotificationMailbox::kHandlerOffset));
    EXPECT_TRUE(NotificationMailbox::MatchesDestOffset(NotificationMailbox::kLegacyHandlerOffset));
    EXPECT_FALSE(NotificationMailbox::MatchesDestOffset(0x000100000004ULL));
}

TEST(DICEDuplexBringupControllerTests, ProtocolRegisterIOUsesNegotiatedSpeedAndDiceReaderUsesFullGlobalReadSize) {
    RecordingFireWireBus bus;
    ProtocolRegisterIO io(bus, bus, 0x02);
    DICETransaction tx(io);

    std::optional<AsyncStatus> readStatus;
    io.ReadQuadBE(MakeDICEAddress(kTxSectionOffset + TxOffset::kSize),
                  [&readStatus](AsyncStatus status, uint32_t value) {
                       readStatus = status;
                       EXPECT_EQ(value, kTxEntryQuadlets);
                   });
    ASSERT_TRUE(readStatus.has_value());
    EXPECT_EQ(*readStatus, AsyncStatus::kSuccess);
    ASSERT_FALSE(bus.Operations().empty());
    EXPECT_EQ(bus.Operations().front().speed, FwSpeed::S400);

    bus.ClearOperations();
    std::optional<IOReturn> globalStatus;
    tx.ReadGlobalStateFull(MakeGeneralSections(),
                           [&globalStatus](IOReturn status, const ASFW::Audio::DICE::GlobalState& state) {
                               globalStatus = status;
                               EXPECT_EQ(state.clockSelect, 0U);
                           });
    ASSERT_TRUE(globalStatus.has_value());
    EXPECT_EQ(*globalStatus, kIOReturnSuccess);
    ASSERT_EQ(bus.Operations().size(), 1U);
    EXPECT_EQ(bus.Operations()[0].addressLo, 0xE0000028U);
    EXPECT_EQ(bus.Operations()[0].length, kGlobalBytes);
    EXPECT_EQ(bus.Operations()[0].speed, FwSpeed::S400);
}

TEST(DICEDuplexBringupControllerTests, ProtocolRegisterIOReadQuadPropagatesTimeout) {
    RecordingFireWireBus bus;
    ProtocolRegisterIO io(bus, bus, 0x02);

    static constexpr std::array<ExpectedRequest, 1> kRequests{{
        {OpKind::Read, 0xFFFFU, 0xE00001A8U, 4U, FwSpeed::S400, 0U, {nullptr, 0U}},
    }};
    static constexpr std::array<ResponseStep, 1> kResponses{{
        {OpKind::Read, 0xFFFFU, 0xE00001A8U, 4U, 0U, FwSpeed::S400, AsyncStatus::kTimeout, {nullptr, 0U}},
    }};
    bus.SetScript(kRequests, kResponses);

    std::optional<AsyncStatus> readStatus;
    io.ReadQuadBE(MakeDICEAddress(kTxSectionOffset + TxOffset::kSize),
                  [&readStatus](AsyncStatus status, uint32_t value) {
                      readStatus = status;
                      EXPECT_EQ(value, 0U);
                  });

    ASSERT_TRUE(readStatus.has_value());
    EXPECT_EQ(*readStatus, AsyncStatus::kTimeout);
    EXPECT_TRUE(bus.ScriptConsumed());
}

TEST(DICEDuplexBringupControllerTests, ProtocolRegisterIOReadQuadPropagatesShortRead) {
    RecordingFireWireBus bus;
    ProtocolRegisterIO io(bus, bus, 0x02);

    static constexpr std::array<uint8_t, 2> kShortPayload{{0x00, 0x46}};
    static constexpr std::array<ExpectedRequest, 1> kRequests{{
        {OpKind::Read, 0xFFFFU, 0xE00001A8U, 4U, FwSpeed::S400, 0U, {nullptr, 0U}},
    }};
    static constexpr std::array<ResponseStep, 1> kResponses{{
        {OpKind::Read, 0xFFFFU, 0xE00001A8U, 4U, 0U, FwSpeed::S400, AsyncStatus::kSuccess,
         {kShortPayload.data(), kShortPayload.size()}},
    }};
    bus.SetScript(kRequests, kResponses);

    std::optional<AsyncStatus> readStatus;
    io.ReadQuadBE(MakeDICEAddress(kTxSectionOffset + TxOffset::kSize),
                  [&readStatus](AsyncStatus status, uint32_t value) {
                      readStatus = status;
                      EXPECT_EQ(value, 0U);
                  });

    ASSERT_TRUE(readStatus.has_value());
    EXPECT_EQ(*readStatus, AsyncStatus::kShortRead);
    EXPECT_TRUE(bus.ScriptConsumed());
}

TEST(DICEDuplexBringupControllerTests, ProtocolRegisterIOWriteQuadUsesNegotiatedSpeedAndBigEndianPayload) {
    RecordingFireWireBus bus;
    ProtocolRegisterIO io(bus, bus, 0x02);

    std::optional<AsyncStatus> writeStatus;
    io.WriteQuadBE(MakeDICEAddress(kTxSectionOffset + TxOffset::kIsochronous),
                   0x00000001U,
                   [&writeStatus](AsyncStatus status) { writeStatus = status; });

    ASSERT_TRUE(writeStatus.has_value());
    EXPECT_EQ(*writeStatus, AsyncStatus::kSuccess);
    ASSERT_EQ(bus.Operations().size(), 1U);
    EXPECT_EQ(bus.Operations()[0].kind, OpKind::Write);
    EXPECT_EQ(bus.Operations()[0].addressLo, 0xE00001ACU);
    EXPECT_EQ(bus.Operations()[0].speed, FwSpeed::S400);
    ASSERT_EQ(bus.Operations()[0].payload.size(), 4U);
    EXPECT_EQ(ASFW::FW::ReadBE32(bus.Operations()[0].payload.data()), 1U);
}

TEST(DICEDuplexBringupControllerTests, ProtocolRegisterIOCompareSwap64UsesLockAndDecodesBigEndianPayload) {
    RecordingFireWireBus bus;
    ProtocolRegisterIO io(bus, bus, 0x02);
    const auto ownerOffset = MakeGeneralSections().global.offset + GlobalOffset::kOwnerHi;

    std::optional<AsyncStatus> lockStatus;
    std::optional<uint64_t> previousOwner;
    io.CompareSwap64BE(MakeDICEAddress(ownerOffset),
                       kOwnerNoOwner,
                       0xFFC0000100000000ULL,
                       [&lockStatus, &previousOwner](AsyncStatus status, uint64_t value) {
                           lockStatus = status;
                           previousOwner = value;
                       });

    ASSERT_TRUE(lockStatus.has_value());
    ASSERT_TRUE(previousOwner.has_value());
    EXPECT_EQ(*lockStatus, AsyncStatus::kSuccess);
    EXPECT_EQ(*previousOwner, kOwnerNoOwner);
    ASSERT_EQ(bus.Operations().size(), 1U);
    EXPECT_EQ(bus.Operations()[0].kind, OpKind::Lock);
    EXPECT_EQ(bus.Operations()[0].addressLo, 0xE0000028U);
    EXPECT_EQ(bus.Operations()[0].speed, FwSpeed::S400);
    EXPECT_EQ(bus.Owner(), 0xFFC0000100000000ULL);
}

TEST(DICEDuplexBringupControllerTests, PrepareSequenceMatchesReferenceWindow) {
    DuplexRig rig;
    NotificationMailbox::Reset();
    rig.bus.SetScript(ReferencePhase0ParityFixture::kPrepareExpectedRequests,
                      ReferencePhase0ParityFixture::kPrepareResponseSteps);

    std::optional<IOReturn> startStatus;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };
    rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });

    ASSERT_TRUE(startStatus.has_value());
    EXPECT_EQ(*startStatus, kIOReturnSuccess);
    EXPECT_TRUE(rig.controller.IsPrepared());
    EXPECT_TRUE(rig.controller.IsOwnerClaimed());
    EXPECT_EQ(rig.bus.Owner(), 0xFFC0000100000000ULL);
    EXPECT_EQ(rig.bus.BandwidthAvailable(), 4019U);
    EXPECT_EQ(rig.bus.ChannelsAvailable31_0(), 0x3FFFFFFFU);
    ExpectRequests(rig.bus.Operations(), ReferencePhase0ParityFixture::kPrepareExpectedRequests);
    EXPECT_TRUE(rig.bus.ScriptConsumed());

    for (const auto& op : rig.bus.Operations()) {
        EXPECT_FALSE(op.addressHi == 0xFFFF && (op.addressLo & 0xFFF00000U) == 0xE0200000U);
    }
}

TEST(DICEDuplexBringupControllerTests, ProgramTxEnableWritesGlobalEnableOnce) {
    DuplexRig rig;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };

    std::optional<IOReturn> startStatus;
    rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });
    ASSERT_TRUE(startStatus.has_value());
    ASSERT_EQ(*startStatus, kIOReturnSuccess);

    rig.bus.ClearOperations();
    const auto txEnableRequests = Concat<ExpectedRequest>(
        ReferencePhase0ParityFixture::kProgramRxExpectedRequests,
        ReferencePhase0ParityFixture::kProgramTxEnableExpectedRequests);
    const auto txEnableResponses = Concat<ResponseStep>(
        ReferencePhase0ParityFixture::kProgramRxResponseSteps,
        ReferencePhase0ParityFixture::kProgramTxEnableResponseSteps);
    rig.bus.SetScript(txEnableRequests, txEnableResponses);
    std::optional<IOReturn> rxStatus;
    rig.controller.ProgramRxForDuplex48k([&rxStatus](IOReturn status) { rxStatus = status; });

    ASSERT_TRUE(rxStatus.has_value());
    ASSERT_EQ(*rxStatus, kIOReturnSuccess);

    std::optional<IOReturn> txEnableStatus;
    rig.controller.ProgramTxAndEnableDuplex48k([&txEnableStatus](IOReturn status) { txEnableStatus = status; });

    ASSERT_TRUE(txEnableStatus.has_value());
    EXPECT_EQ(*txEnableStatus, kIOReturnSuccess);
    ExpectRequests(rig.bus.Operations(), txEnableRequests);
    EXPECT_TRUE(rig.bus.ScriptConsumed());
    EXPECT_EQ(rig.bus.Enable(), 1U);
}

TEST(DICEDuplexBringupControllerTests, ProgramRxMatchesReferenceSegment) {
    DuplexRig rig;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };

    std::optional<IOReturn> startStatus;
    rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });
    ASSERT_TRUE(startStatus.has_value());
    ASSERT_EQ(*startStatus, kIOReturnSuccess);

    rig.bus.ClearOperations();
    rig.bus.SetScript(ReferencePhase0ParityFixture::kProgramRxExpectedRequests,
                      ReferencePhase0ParityFixture::kProgramRxResponseSteps);

    std::optional<IOReturn> rxStatus;
    rig.controller.ProgramRxForDuplex48k([&rxStatus](IOReturn status) { rxStatus = status; });

    ASSERT_TRUE(rxStatus.has_value());
    EXPECT_EQ(*rxStatus, kIOReturnSuccess);
    ExpectRequests(rig.bus.Operations(), ReferencePhase0ParityFixture::kProgramRxExpectedRequests);
    ASSERT_GE(rig.bus.Operations().size(), 3U);
    EXPECT_EQ(rig.bus.Operations()[0].kind, OpKind::Read);
    EXPECT_EQ(rig.bus.Operations()[0].addressLo, 0xE00003E0U);
    EXPECT_EQ(rig.bus.Operations()[1].kind, OpKind::Write);
    EXPECT_EQ(rig.bus.Operations()[1].addressLo, 0xE00003E4U);
    EXPECT_EQ(rig.bus.Operations()[2].kind, OpKind::Write);
    EXPECT_EQ(rig.bus.Operations()[2].addressLo, 0xE00003E8U);
    EXPECT_EQ(rig.bus.Enable(), 0U);
}

TEST(DICEDuplexBringupControllerTests, StopSequenceReleasesOwnerLast) {
    DuplexRig rig;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };

    std::optional<IOReturn> startStatus;
    rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });
    ASSERT_TRUE(startStatus.has_value());
    ASSERT_EQ(*startStatus, kIOReturnSuccess);

    rig.bus.ClearOperations();
    const IOReturn stopStatus = rig.controller.StopDuplex();
    EXPECT_EQ(stopStatus, kIOReturnSuccess);
    EXPECT_FALSE(rig.controller.IsPrepared());
    EXPECT_FALSE(rig.controller.IsOwnerClaimed());
    EXPECT_EQ(rig.bus.Owner(), kOwnerNoOwner);
    EXPECT_EQ(rig.bus.BandwidthAvailable(), 4019U);
    EXPECT_EQ(rig.bus.ChannelsAvailable31_0(), 0x3FFFFFFFU);
    ExpectOperations(rig.bus.Operations(), ExpectedStopOps());
}

TEST(DICEDuplexBringupControllerTests, LateClockAcceptedNotifyDoesNotTriggerRollback) {
    NotificationMailbox::Reset();
    HostClockResetGuard clockReset;

    uint64_t nowNs = 0;
    ASFW::Testing::SetHostMonotonicClockForTesting([&nowNs]() { return nowNs; });

    RecordingFireWireBus bus;
    IODispatchQueue queue;
    queue.SetManualDispatchForTesting(true);
    bus.SetClockSelectWriteHandler([&queue, &bus]() {
        queue.DispatchAsyncAfter(3'250'000'000ULL, [&bus]() { bus.PublishClockAccepted(); });
    });

    ProtocolRegisterIO io(bus, bus, 0x02);
    DICETransaction tx(io);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    DICEDuplexBringupController controller(tx, io, bus, &queue, MakeGeneralSections());

    std::optional<IOReturn> startStatus;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };

    controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });
    EXPECT_FALSE(startStatus.has_value());

    for (size_t i = 0; i < 600 && !startStatus.has_value(); ++i) {
        nowNs += 10'000'000ULL;
        while (queue.DrainReadyForTesting() > 0) {
        }
    }

    ASSERT_TRUE(startStatus.has_value());
    EXPECT_EQ(*startStatus, kIOReturnSuccess);
    EXPECT_TRUE(controller.IsPrepared());
    ExpectRequests(bus.Operations(), ReferencePhase0ParityFixture::kPrepareExpectedRequests);
}

TEST(DICEDuplexBringupControllerTests, GlobalStateConfirmationRecoversIfMailboxMissesClockAccepted) {
    NotificationMailbox::Reset();
    HostClockResetGuard clockReset;

    uint64_t nowNs = 0;
    ASFW::Testing::SetHostMonotonicClockForTesting([&nowNs]() { return nowNs; });

    RecordingFireWireBus bus;
    IODispatchQueue queue;
    queue.SetManualDispatchForTesting(true);
    bus.SetClockSelectWriteHandler([&bus]() { bus.LatchClockAccepted(); });

    ProtocolRegisterIO io(bus, bus, 0x02);
    DICETransaction tx(io);
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    DICEDuplexBringupController controller(tx, io, bus, &queue, MakeGeneralSections());

    std::optional<IOReturn> startStatus;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };

    controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });
    EXPECT_FALSE(startStatus.has_value());

    for (size_t i = 0; i < 700 && !startStatus.has_value(); ++i) {
        nowNs += 10'000'000ULL;
        while (queue.DrainReadyForTesting() > 0) {
        }
    }

    ASSERT_TRUE(startStatus.has_value());
    EXPECT_EQ(*startStatus, kIOReturnSuccess);
    EXPECT_TRUE(controller.IsPrepared());
    ExpectRequests(bus.Operations(), ReferencePhase0ParityFixture::kPrepareExpectedRequests);
}

TEST(DICEDuplexBringupControllerTests, IRMReadResourcesSnapshotUsesQuadletReads) {
    RecordingFireWireBus bus;
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<ASFW::IRM::AllocationStatus> status;
    ASFW::IRM::ResourceSnapshot snapshot{};
    irm.ReadResourcesSnapshot([&](ASFW::IRM::AllocationStatus s, ASFW::IRM::ResourceSnapshot value) {
        status = s;
        snapshot = value;
    });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, ASFW::IRM::AllocationStatus::Success);
    EXPECT_EQ(snapshot.bandwidthAvailable, 4019U);
    EXPECT_EQ(snapshot.channelsAvailable31_0, 0x3FFFFFFFU);
    EXPECT_EQ(snapshot.channelsAvailable63_32, 0xFFFFFFFFU);

    const std::vector<ExpectedOp> expected{
        {OpKind::Read, 0xF0000220U, 4, FwSpeed::S100},
        {OpKind::Read, 0xF0000224U, 4, FwSpeed::S100},
        {OpKind::Read, 0xF0000228U, 4, FwSpeed::S100},
    };
    ExpectOperations(bus.Operations(), expected);
}

TEST(DICEDuplexBringupControllerTests, IRMAllocateResourcesUsesSnapshotThenCompareSwap) {
    RecordingFireWireBus bus;
    bus.SetIRMResourceState(4915U, 0xFFFFFFFFU, 0xFFFFFFFFU);

    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<ASFW::IRM::AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](ASFW::IRM::AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, ASFW::IRM::AllocationStatus::Success);
    EXPECT_EQ(bus.BandwidthAvailable(), 4595U);
    EXPECT_EQ(bus.ChannelsAvailable31_0(), 0x7FFFFFFFU);

    const std::vector<ExpectedOp> expected{
        {OpKind::Read, 0xF0000220U, 4, FwSpeed::S100},
        {OpKind::Read, 0xF0000224U, 4, FwSpeed::S100},
        {OpKind::Read, 0xF0000228U, 4, FwSpeed::S100},
        {OpKind::Lock, 0xF0000220U, 8, FwSpeed::S100},
        {OpKind::Lock, 0xF0000224U, 8, FwSpeed::S100},
    };
    ExpectOperations(bus.Operations(), expected);
}

TEST(DICEDuplexBringupControllerTests, IRMPlaybackAllocationMatchesGeneratedReferenceSegment) {
    RecordingFireWireBus bus;
    bus.SetScript(ReferencePhase0ParityFixture::kIrmPlaybackExpectedRequests,
                  ReferencePhase0ParityFixture::kIrmPlaybackResponseSteps);

    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<ASFW::IRM::AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](ASFW::IRM::AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, ASFW::IRM::AllocationStatus::Success);
    ExpectRequests(bus.Operations(), ReferencePhase0ParityFixture::kIrmPlaybackExpectedRequests);
    EXPECT_TRUE(bus.ScriptConsumed());
}

TEST(DICEDuplexBringupControllerTests, IRMCaptureAllocationMatchesGeneratedReferenceSegment) {
    RecordingFireWireBus bus;
    bus.SetScript(ReferencePhase0ParityFixture::kIrmCaptureExpectedRequests,
                  ReferencePhase0ParityFixture::kIrmCaptureResponseSteps);

    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});

    std::optional<ASFW::IRM::AllocationStatus> status;
    irm.AllocateResources(1, 576U, [&](ASFW::IRM::AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, ASFW::IRM::AllocationStatus::Success);
    ExpectRequests(bus.Operations(), ReferencePhase0ParityFixture::kIrmCaptureExpectedRequests);
    EXPECT_TRUE(bus.ScriptConsumed());
}

TEST(DICEDuplexBringupControllerTests, IRMAllocateResourcesReturnsGenerationMismatchWhenBusMoves) {
    RecordingFireWireBus bus;
    IRMClient irm(bus);
    irm.SetIRMNode(0x03, Generation{1});
    bus.SetGeneration(Generation{2});

    std::optional<ASFW::IRM::AllocationStatus> status;
    irm.AllocateResources(0, 320U, [&](ASFW::IRM::AllocationStatus s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, ASFW::IRM::AllocationStatus::GenerationMismatch);
}
