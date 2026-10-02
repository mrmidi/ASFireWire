// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcSimulatedUnitTests.cpp - Tests for SimulatedAvcUnit and RecordingFireWireBus FCP integration.

#include <gtest/gtest.h>
#include "ASFWDriver/Common/OnceCompletion.hpp"

#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/FunctionBlockCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Descriptors/DescriptorAccessor.hpp"
#include "ASFWDriver/Protocols/AVC/Descriptors/MusicSubunitDescriptor.hpp"
#include "ASFWDriver/Protocols/AVC/Descriptors/AudioSubunitDescriptor.hpp"
#include "ASFWDriver/Protocols/AVC/Audio/AudioSubunit.hpp"
#include "ASFWDriver/Protocols/AVC/Core/IAvcUnit.hpp"
#include "ASFWDriver/Protocols/AVC/Extensions/BridgeCoPlugInfo.hpp"
#include "RecordingFireWireBus.hpp"
#include "SimulatedAvcUnit.hpp"
#include "Phase88DescriptorFixtures.hpp"

namespace ASFW::AVC::Testing {

class AvcSimulatedUnitTests : public ::testing::Test {
protected:
    SimulatedAvcUnit duetUnit_{kDuet};
    SimulatedAvcUnit phase88Unit_{kPhase88};
    ::ASFW::Testing::RecordingFireWireBus bus_;
};

TEST_F(AvcSimulatedUnitTests, DuetUnitInfoDiscovery) {
    EXPECT_EQ(duetUnit_.Guid(), kDuet.guid);
    EXPECT_EQ(duetUnit_.NodeId().value, 0);

    std::optional<Expected<Cmd::UnitInfo>> unitInfo;
    duetUnit_.Status(Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> res) {
        unitInfo = res;
    });

    ASSERT_TRUE(unitInfo.has_value());
    ASSERT_TRUE(unitInfo->has_value());
    EXPECT_EQ((*unitInfo)->companyId[0], 0x00);
    EXPECT_EQ((*unitInfo)->companyId[1], 0x03);
    EXPECT_EQ((*unitInfo)->companyId[2], 0xDB); // Apogee OUI
}

TEST_F(AvcSimulatedUnitTests, DuetSubunitAndPlugInfo) {
    std::optional<Expected<Cmd::SubunitInfo>> subunitInfo;
    duetUnit_.Status(Cmd::SubunitInfoCommand{.operands = Cmd::SubunitInfoOperands{.page = 0}}, [&](Expected<Cmd::SubunitInfo> res) {
        subunitInfo = res;
    });

    ASSERT_TRUE(subunitInfo.has_value());
    ASSERT_TRUE(subunitInfo->has_value());
    EXPECT_EQ((*subunitInfo)->entryCount, 2);
    EXPECT_EQ((*subunitInfo)->entries[0].type, SubunitType::kAudio);
    EXPECT_EQ((*subunitInfo)->entries[1].type, SubunitType::kMusic);

    std::optional<Expected<Cmd::PlugInfoReply>> plugInfo;
    duetUnit_.Status(Cmd::PlugInfoCommand{}, [&](Expected<Cmd::PlugInfoReply> res) {
        plugInfo = res;
    });

    ASSERT_TRUE(plugInfo.has_value());
    ASSERT_TRUE(plugInfo->has_value());
    EXPECT_EQ((*plugInfo)->unit.isochronousInputs, 1);
    EXPECT_EQ((*plugInfo)->unit.isochronousOutputs, 1);
}

TEST_F(AvcSimulatedUnitTests, BeBoBPlugDiscoveryCalls) {
    std::optional<Expected<Cmd::PlugInfoReply>> counts;
    phase88Unit_.Status(Cmd::PlugInfoCommand{}, [&](Expected<Cmd::PlugInfoReply> reply) {
        counts = reply;
    });
    ASSERT_TRUE(counts && counts->has_value());
    EXPECT_EQ((*counts)->unit.isochronousInputs, 2);

    std::optional<Expected<Cmd::PlugInfoReply>> asyncCounts;
    phase88Unit_.Status(Cmd::PlugInfoCommand{
        .operands = {.form = Cmd::PlugInfoForm::kUnitAsync},
    }, [&](Expected<Cmd::PlugInfoReply> reply) { asyncCounts = reply; });
    ASSERT_TRUE(asyncCounts && asyncCounts->has_value());
    EXPECT_EQ((*asyncCounts)->asynchronous.asynchronousInputs, 0);

    std::optional<Expected<Cmd::PlugInfoReply>> audioCounts;
    phase88Unit_.Status(Cmd::PlugInfoCommand{
        .address = kAudioSubunit0,
        .operands = {.form = Cmd::PlugInfoForm::kSubunit},
    }, [&](Expected<Cmd::PlugInfoReply> reply) { audioCounts = reply; });
    ASSERT_TRUE(audioCounts && audioCounts->has_value());
    EXPECT_EQ((*audioCounts)->subunit.destinationPlugs, 8);

    // The recorded Phase 88 probe returned NOT IMPLEMENTED for this extension.
    // Supply a BridgeCo reply to exercise the typed discovery call itself.
    phase88Unit_.SetResponseOverride(
        {0x01, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x02},
        {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x02, 0x08});
    std::optional<Expected<BridgeCo::ExtendedPlugInfoReply>> channelCount;
    phase88Unit_.Status(BridgeCo::ExtendedPlugInfoCommand{
        .operands = {.plug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kInput,
                                                       Cmd::UnitPlugType::kPcr, 0),
                     .type = BridgeCo::InfoType::kChannelCount},
    }, [&](Expected<BridgeCo::ExtendedPlugInfoReply> reply) { channelCount = reply; });
    ASSERT_TRUE(channelCount && channelCount->has_value());
    ASSERT_TRUE((*channelCount)->AsChannelCount().has_value());
    EXPECT_EQ(*(*channelCount)->AsChannelCount(), 8);
}

TEST_F(AvcSimulatedUnitTests, Phase88MasterVolumeControlCall) {
    // FB1 master channel, -35 dB in signed 1/256 dB units.
    phase88Unit_.SetResponseOverride(
        {0x00, 0x08, 0xB8, 0x81, 0x01, 0x10, 0x02, 0x00, 0x02, 0x02, 0xDD, 0x00},
        {0x09, 0x08, 0xB8, 0x81, 0x01, 0x10, 0x02, 0x00, 0x02, 0x02, 0xDD, 0x00});
    std::optional<Expected<Cmd::FeatureReply>> volume;
    phase88Unit_.Control(Cmd::FeatureCommand{
        .address = kAudioSubunit0,
        .operands = Cmd::FeatureOperands::Volume(1, Cmd::kMasterChannel,
                                                 AvcVolume::FromDb(-35.0f)),
    }, [&](Expected<Cmd::FeatureReply> reply) { volume = reply; });
    ASSERT_TRUE(volume && volume->has_value());
    EXPECT_EQ((*volume)->AsVolume().Raw(), AvcVolume::FromDb(-35.0f).Raw());
}

TEST_F(AvcSimulatedUnitTests, DuetPcmVendorCall) {
    const uint8_t pcm[] = {0x50, 0x43, 0x4D, 0x15, 0x80, 0xFF};
    std::optional<Expected<Cmd::RawVendorDependentReply>> vendorReply;
    duetUnit_.Status(Cmd::RawVendorDependentCommand{
        .operands = Cmd::RawVendorDependentOperands({0x00, 0x03, 0xDB}, pcm),
    }, [&](Expected<Cmd::RawVendorDependentReply> reply) { vendorReply = reply; });
    ASSERT_TRUE(vendorReply && vendorReply->has_value());
    EXPECT_EQ((*vendorReply)->companyId, (CompanyId{0x00, 0x03, 0xDB}));
    ASSERT_GE((*vendorReply)->payload.size(), 3u);
    EXPECT_EQ((*vendorReply)->payload[0], 0x50);
    EXPECT_EQ((*vendorReply)->payload[1], 0x43);
    EXPECT_EQ((*vendorReply)->payload[2], 0x4D);
}

TEST_F(AvcSimulatedUnitTests, SignalFormatStatusCall) {
    std::optional<Expected<Cmd::PlugSignalFormat>> format;
    duetUnit_.Status(Cmd::PlugSignalFormatCommand{
        .operands = {.direction = Cmd::PlugSignalDirection::kInput, .plugId = 0},
    }, [&](Expected<Cmd::PlugSignalFormat> reply) { format = reply; });
    ASSERT_TRUE(format && format->has_value());
    EXPECT_EQ((*format)->fmt, Cmd::kFmtAm824);
}

TEST_F(AvcSimulatedUnitTests, Phase88StreamFormatQuery) {
    EXPECT_EQ(phase88Unit_.Guid(), 0x000AAC0300B1D1F7ULL);

    // Query single format for Iso In 0 via 0x2F
    auto cmd = Cmd::StreamFormatCommand{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::StreamFormatOperands{
            .opcode = Cmd::StreamFormatOpcode::kStreamFormatSupport,
            .plug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kInput, Cmd::UnitPlugType::kPcr, 0),
        },
    }.Encode(CommandType::kStatus);
    ASSERT_TRUE(cmd.has_value());

    std::optional<Expected<Response>> rawResponse;
    phase88Unit_.Submit(*cmd, phase88Unit_.CurrentGeneration(), [&](Expected<Response> res) {
        rawResponse = res;
    });

    ASSERT_TRUE(rawResponse.has_value());
    ASSERT_TRUE(rawResponse->has_value());
    auto singleFmt = Cmd::StreamFormatOperands{}.Read((*rawResponse)->operands);
    ASSERT_TRUE(singleFmt.has_value());
    EXPECT_EQ(singleFmt->format.compound.rate, StreamFormatRate::k48000);
}

TEST_F(AvcSimulatedUnitTests, Phase88AudioDescriptorTraversalUsesSessionsAndRetainsDeferredOwners) {
    phase88Unit_.SetDescriptor(0x08, {0x00}, Fixtures::kPhase88AudioIdentifier);
    phase88Unit_.SetDescriptor(0x08, {0x10, 0x18, 0x00}, Fixtures::kPhase88TextRoot);
    phase88Unit_.SetDescriptor(0x08, {0x10, 0x18, 0x01}, Fixtures::kPhase88TextChild);

    // The simulator requires OPEN before READ and defers each response, so every
    // chunk and nested descriptor callback runs after its submitting frame returns.
    Protocols::AVC::DescriptorAccessor unopened(phase88Unit_, SubunitAddress::FromByte(0x08));
    std::optional<Protocols::AVC::DescriptorAccessor::ReadDescriptorResult> unopenedResult;
    unopened.readComplete(Protocols::AVC::DescriptorSpecifier::forUnitIdentifier(),
        [&](const auto& result) { unopenedResult = result; });
    ASSERT_TRUE(unopenedResult.has_value());
    EXPECT_TRUE(unopenedResult->success);

    phase88Unit_.SetDeferredResponses(true);
    Protocols::AVC::Audio::AudioSubunit audio(Protocols::AVC::AVCSubunitType::kAudio, 0);
    std::optional<bool> completed;
    audio.ReadIdentifierDescriptor(phase88Unit_, [&](bool ok) { completed = ok; });
    for (size_t i = 0; i < 80 && !completed; ++i) {
        phase88Unit_.FlushDeferredResponses();
    }
    ASSERT_TRUE(completed.has_value());
    EXPECT_TRUE(*completed);
    ASSERT_TRUE(audio.GetIdentifier().has_value());
    const auto* master = audio.GetIdentifier()->FindBlock(
        Descriptors::AudioFunctionBlockType::kFeature, 1);
    ASSERT_NE(master, nullptr);
    EXPECT_EQ(master->name, "Mixer Output Level");
    const auto* input = audio.GetIdentifier()->FindBlock(
        Descriptors::AudioFunctionBlockType::kFeature, 2);
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->name, "Mixer Input LineIn 1/2 Level");
    phase88Unit_.SetDeferredResponses(false);
}

TEST_F(AvcSimulatedUnitTests, DescriptorReadRejectsPrematureEmptyChunk) {
    phase88Unit_.SetDescriptor(0x08, {0x10, 0x12, 0x34},
                               {0x00, 0x14, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00});
    Protocols::AVC::DescriptorAccessor accessor(phase88Unit_, SubunitAddress::FromByte(0x08));
    auto specifier = Protocols::AVC::DescriptorSpecifier{
        .type = Protocols::AVC::DescriptorSpecifierType::kListID,
        .typeSpecificFields = {0x12, 0x34},
    };
    std::optional<Protocols::AVC::DescriptorAccessor::ReadDescriptorResult> result;
    accessor.readWithOpenCloseSequence(specifier,
        [&](const auto& read) { result = read; });
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->success);
    EXPECT_EQ(result->avcResult, Protocols::AVC::AVCResult::kInvalidResponse);
}

TEST_F(AvcSimulatedUnitTests, FaultKnobs_TimeoutAndInterimAndRejections) {
    // 1. Timeout
    duetUnit_.SetTimeoutNext(true);
    bool called = false;
    duetUnit_.Status(Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo>) {
        called = true;
    });
    EXPECT_FALSE(called); // Dropped

    // 2. Interim deferral
    duetUnit_.SetInterimNext(true);
    std::vector<ResponseCode> codes;
    auto cmd = Command<Cmd::UnitInfoOperands>{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::UnitInfoOperands{},
    }.Encode(CommandType::kStatus);
    ASSERT_TRUE(cmd.has_value());
    duetUnit_.Submit(*cmd, duetUnit_.CurrentGeneration(), [&](Expected<Response> res) {
        if (res) {
            codes.push_back(res->code);
        }
    });
    ASSERT_EQ(codes.size(), 2u);
    EXPECT_EQ(codes[0], ResponseCode::kInterim);
    EXPECT_EQ(codes[1], ResponseCode::kImplementedStable);

    // 3. Rejection
    duetUnit_.SetRejectNext(true);
    std::optional<Expected<Cmd::UnitInfo>> rejectedResult;
    duetUnit_.Status(Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> res) {
        rejectedResult = res;
    });
    ASSERT_TRUE(rejectedResult.has_value());
    ASSERT_FALSE(rejectedResult->has_value());
    EXPECT_EQ(rejectedResult->error().kind, AvcErrorKind::kUnexpectedResponse);
    EXPECT_EQ(*rejectedResult->error().response, ResponseCode::kRejected);

    // 4. Not Implemented
    duetUnit_.SetNotImplementedNext(true);
    std::optional<Expected<Cmd::UnitInfo>> notImplResult;
    duetUnit_.Status(Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> res) {
        notImplResult = res;
    });
    ASSERT_TRUE(notImplResult.has_value());
    ASSERT_FALSE(notImplResult->has_value());
    EXPECT_EQ(notImplResult->error().kind, AvcErrorKind::kUnexpectedResponse);
    EXPECT_EQ(*notImplResult->error().response, ResponseCode::kNotImplemented);

    // 5. Bus Reset
    duetUnit_.SetBusResetNext(true);
    std::optional<Expected<Cmd::UnitInfo>> busResetResult;
    duetUnit_.Status(Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> res) {
        busResetResult = res;
    });
    ASSERT_TRUE(busResetResult.has_value());
    ASSERT_FALSE(busResetResult->has_value());
    EXPECT_EQ(busResetResult->error().kind, AvcErrorKind::kBusReset);
}

TEST_F(AvcSimulatedUnitTests, BusAttachment_FcpCommandInterception) {
    std::vector<uint8_t> deliveredFcpResponse;
    duetUnit_.AttachToBus(bus_, [&](const uint8_t* data, size_t length) {
        deliveredFcpResponse.assign(data, data + length);
    });

    // Write FCP UNIT INFO command to 0xFFFFF0000B00
    const uint8_t unitInfoCommand[] = {0x01, 0xFF, 0x30, 0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    Async::FWAddress fcpCmdAddr{Async::FWAddress::AddressParts{
        .addressHi = 0xFFFF,
        .addressLo = 0xF0000B00,
    }};

    bus_.WriteBlock(FW::Generation{1}, FW::NodeId{0}, fcpCmdAddr,
                    unitInfoCommand, FW::FwSpeed::S100,
                    [](Async::AsyncStatus, std::span<const uint8_t>) {});

    ASSERT_FALSE(deliveredFcpResponse.empty());
    EXPECT_EQ(deliveredFcpResponse[0], 0x0C); // IMPLEMENTED/STABLE
    EXPECT_EQ(deliveredFcpResponse[1], 0xFF); // Unit
    EXPECT_EQ(deliveredFcpResponse[2], 0x30); // UNIT INFO
    EXPECT_EQ(deliveredFcpResponse[5], 0x00); // OUI 0x0003DB
    EXPECT_EQ(deliveredFcpResponse[6], 0x03);
    EXPECT_EQ(deliveredFcpResponse[7], 0xDB);

    // Verify trace recorded the write
    EXPECT_FALSE(bus_.Trace().Lines().empty());
    EXPECT_TRUE(bus_.Trace().Lines()[0].find("W ffff.f0000b00 01ff3007ffffffff") != std::string::npos);
}

TEST_F(AvcSimulatedUnitTests, DescriptorChunkedServing) {
    const std::string duetHex =
        "01ce000a810000060101ffffffff01c08108000403030005002e8109000800900200000100020020810a000b060302000000ff000101ff000f000a000b416e616c6f67204f757400002d810900080190020500010002001f810a000b060302000200ff000301ff000e000a000a416e616c6f6720496e0000248109000802900203000100010016810a0007400901000400ff0009000a000553796e6300002d810900080090020000010002001f810a000b060302000200ff000301ff000e000a000a416e616c6f6720496e00002e8109000801900205000100020020810a000b060302000000ff000101ff000f000a000b416e616c6f67204f75740000248109000802900203000100010016810a0007400901000400ff0009000a000553796e63000025810b000e00000000f000ff00fff101ff00ff0011000a000d416e616c6f67204f75742031000025810b000e00000100f000ff01fff101ff01ff0011000a000d416e616c6f67204f75742032000024810b000e00000200f001ff00fff100ff00ff0010000a000c416e616c6f6720496e2031000024810b000e00000300f001ff01fff100ff01ff0010000a000c416e616c6f6720496e2032000012810b000e80000400f002ff00fff102ff00ff";

    std::vector<uint8_t> descriptorBytes;
    descriptorBytes.reserve(duetHex.size() / 2);
    for (size_t i = 0; i < duetHex.size(); i += 2) {
        descriptorBytes.push_back(static_cast<uint8_t>(std::stoul(duetHex.substr(i, 2), nullptr, 16)));
    }
    ASSERT_EQ(descriptorBytes.size(), 464u);

    // Register descriptor for Music Subunit 0 (subunit address 0x60), Status Descriptor specifier 0x80
    duetUnit_.SetDescriptor(0x60, {0x80}, descriptorBytes);

    // Access descriptor via DescriptorAccessor
    Protocols::AVC::DescriptorAccessor accessor(duetUnit_, SubunitAddress::Of(SubunitType::kMusic, 0));
    std::optional<Protocols::AVC::DescriptorAccessor::ReadDescriptorResult> readResult;
    accessor.readStatusDescriptor(0x80, [&](const Protocols::AVC::DescriptorAccessor::ReadDescriptorResult& res) {
        readResult = res;
    });

    ASSERT_TRUE(readResult.has_value());
    EXPECT_TRUE(readResult->success);
    EXPECT_EQ(readResult->avcResult, Protocols::AVC::AVCResult::kAccepted);
    EXPECT_EQ(readResult->data.size(), 464u);
    EXPECT_EQ(readResult->data, descriptorBytes);

    // Parse the retrieved bytes via MusicSubunitDescriptorParser
    auto parsed = Protocols::AVC::Descriptors::MusicSubunitDescriptorParser::ParseStatusDescriptor(readResult->data);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->declaredLength, 462);
    EXPECT_EQ(parsed->plugs.size(), 6u);
    EXPECT_EQ(parsed->musicPlugs.size(), 5u);

    // Test ClearDescriptors
    duetUnit_.ClearDescriptors();
    std::optional<Protocols::AVC::DescriptorAccessor::ReadDescriptorResult> failedResult;
    accessor.readStatusDescriptor(0x80, [&](const Protocols::AVC::DescriptorAccessor::ReadDescriptorResult& res) {
        failedResult = res;
    });
    ASSERT_TRUE(failedResult.has_value());
    EXPECT_FALSE(failedResult->success);
}

TEST_F(AvcSimulatedUnitTests, AudioSubunitReadIdentifierDescriptor_DuetIntegration) {
    const std::string duetAudioHex =
        "0036000200020000002c002a00010026000000040202c0000181000100188101ffff01f00000040202c00000090802000003000200020000";

    std::vector<uint8_t> descriptorBytes;
    descriptorBytes.reserve(duetAudioHex.size() / 2);
    for (size_t i = 0; i < duetAudioHex.size(); i += 2) {
        descriptorBytes.push_back(static_cast<uint8_t>(std::stoul(duetAudioHex.substr(i, 2), nullptr, 16)));
    }
    ASSERT_EQ(descriptorBytes.size(), 56u);

    // Audio Subunit 0 address is 0x08 (0x01 << 3 | 0), identifier specifier is 0x00
    duetUnit_.SetDescriptor(0x08, {0x00}, descriptorBytes);

    Protocols::AVC::Audio::AudioSubunit audioSubunit(Protocols::AVC::AVCSubunitType::kAudio, 0);
    EXPECT_FALSE(audioSubunit.GetIdentifier().has_value());

    std::optional<bool> readOk;
    audioSubunit.ReadIdentifierDescriptor(duetUnit_, [&](bool success) {
        readOk = success;
    });

    ASSERT_TRUE(readOk.has_value());
    EXPECT_TRUE(*readOk);

    const auto& id = audioSubunit.GetIdentifier();
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->generationId, 0);
    ASSERT_EQ(id->functionBlocks.size(), 1u);
    EXPECT_EQ(id->functionBlocks[0].id, 1);
    EXPECT_EQ(id->functionBlocks[0].type, Protocols::AVC::Descriptors::AudioFunctionBlockType::kFeature);
    EXPECT_EQ(id->functionBlocks[0].clusterChannels, 2);
    EXPECT_TRUE((id->functionBlocks[0].masterControls & Protocols::AVC::Descriptors::FeatureControlMask::kVolume) != 0);
    EXPECT_TRUE((id->functionBlocks[0].masterControls & Protocols::AVC::Descriptors::FeatureControlMask::kMute) != 0);
}

} // namespace ASFW::AVC::Testing

namespace {
namespace Avc = ASFW::AVC;
namespace Legacy = ASFW::Protocols::AVC;
class DescriptorTestUnit final : public Avc::IAvcUnit {
public:
    std::vector<std::vector<uint8_t>> commands;
    std::vector<uint8_t> descriptor{0, 2, 0xAA, 0xBB};
    bool rejectOpen{false}, rejectClose{false}, rejectRead{false}, deferred{false};
    uint32_t generation{1};
    struct Pending { Avc::CommandFrame frame; ResponseCallback callback; };
    std::vector<Pending> pending;
    ASFW::FW::NodeId NodeId() const noexcept override { return ASFW::FW::NodeId{1}; }
    ASFW::FW::Generation CurrentGeneration() const noexcept override { return ASFW::FW::Generation{generation}; }
    uint64_t Guid() const noexcept override { return 1; }
    void Submit(const Avc::CommandFrame& frame, ASFW::FW::Generation, ResponseCallback callback) override {
        commands.emplace_back(frame.Bytes().begin(), frame.Bytes().end());
        if (deferred) pending.push_back(Pending{frame, std::move(callback)});
        else Reply(frame, std::move(callback));
    }
    void FlushOne() {
        ASSERT_FALSE(pending.empty());
        auto item = std::move(pending.front()); pending.erase(pending.begin());
        Reply(item.frame, std::move(item.callback));
    }
    void Reply(const Avc::CommandFrame& frame, ResponseCallback callback) {
        const auto command = frame.Bytes();
        std::vector<uint8_t> response(command.begin(), command.end());
        response[0] = 9;
        if (command[2] == 8) {
            if ((command[4] == 1 && rejectOpen) || (command[4] == 0 && rejectClose)) response[0] = 10;
        } else if (command[2] == 9) {
            if (rejectRead) response[0] = 10;
            else {
                const size_t offset = (static_cast<size_t>(command[8]) << 8) | command[9];
                const size_t requested = (static_cast<size_t>(command[6]) << 8) | command[7];
                const size_t count = offset <= descriptor.size() ? std::min(requested, descriptor.size() - offset) : 0;
                response.resize(10);
                response[4] = 0x11; // Deliberately inaccurate at EOF: length wins.
                response[6] = static_cast<uint8_t>(count >> 8); response[7] = static_cast<uint8_t>(count);
                response.insert(response.end(), descriptor.begin() + offset, descriptor.begin() + offset + count);
            }
        }
        callback(Avc::ParseResponseFor(frame, response));
    }
};
using ReadResult = Legacy::DescriptorAccessor::ReadDescriptorResult;
TEST(DescriptorOperation, NoReadWithoutSuccessfulOpen) {
    DescriptorTestUnit unit; unit.rejectOpen = true;
    Legacy::DescriptorAccessor accessor(unit);
    std::optional<ReadResult> result;
    accessor.readUnitIdentifier([&](const auto& value) { result = value; });
    ASSERT_TRUE(result); EXPECT_FALSE(result->success);
    ASSERT_EQ(unit.commands.size(), 1); EXPECT_EQ(unit.commands[0][2], 8);
}
TEST(DescriptorOperation, DeclaredLengthWinsAndCloseFailureIsSeparate) {
    DescriptorTestUnit unit; unit.rejectClose = true;
    Legacy::DescriptorAccessor accessor(unit);
    std::optional<ReadResult> result;
    accessor.readUnitIdentifier([&](const auto& value) { result = value; });
    ASSERT_TRUE(result); EXPECT_TRUE(result->success); EXPECT_EQ(result->data, unit.descriptor);
    EXPECT_FALSE(result->primaryError); ASSERT_TRUE(result->cleanupError);
    ASSERT_EQ(unit.commands.size(), 3); EXPECT_EQ(unit.commands[2][4], 0);
}
TEST(DescriptorOperation, FailedReadStillCloses) {
    DescriptorTestUnit unit; unit.rejectRead = true;
    Legacy::DescriptorAccessor accessor(unit);
    std::optional<ReadResult> result;
    accessor.readUnitIdentifier([&](const auto& value) { result = value; });
    ASSERT_TRUE(result); EXPECT_FALSE(result->success); EXPECT_TRUE(result->primaryError);
    ASSERT_EQ(unit.commands.size(), 3); EXPECT_EQ(unit.commands.back()[4], 0);
}
TEST(DescriptorOperation, ResetDuringReadDoesNotCloseNewRoute) {
    DescriptorTestUnit unit; unit.deferred = true;
    Legacy::DescriptorAccessor accessor(unit);
    size_t completions = 0;
    accessor.readUnitIdentifier([&](const auto& value) { ++completions; EXPECT_TRUE(value.cancelled); });
    unit.FlushOne(); // successful OPEN, queued READ
    ++unit.generation;
    unit.FlushOne();
    EXPECT_EQ(completions, 1); EXPECT_EQ(unit.commands.size(), 2);
}
TEST(DescriptorOperation, DestroyAccessorDuringOpenStillClosesWithoutReading) {
    DescriptorTestUnit unit; unit.deferred = true;
    size_t completions = 0;
    auto accessor = std::make_unique<Legacy::DescriptorAccessor>(unit);
    accessor->readUnitIdentifier([&](const auto& value) { ++completions; EXPECT_TRUE(value.cancelled); });
    accessor.reset();
    unit.FlushOne(); // OPEN succeeds after cancellation: only CLOSE may follow.
    ASSERT_EQ(unit.commands.size(), 2); EXPECT_EQ(unit.commands.back()[2], 8); EXPECT_EQ(unit.commands.back()[4], 0);
    unit.FlushOne();
    EXPECT_EQ(completions, 1);
}
TEST(DescriptorOperation, DuplicateRequestIsBusyAndDoesNotSubmit) {
    DescriptorTestUnit unit; unit.deferred = true;
    Legacy::DescriptorAccessor accessor(unit);
    accessor.readUnitIdentifier([](const auto&) {});
    accessor.readUnitIdentifier([](const auto& value) { EXPECT_EQ(value.avcResult, Legacy::AVCResult::kBusy); });
    EXPECT_EQ(unit.commands.size(), 1);
    unit.FlushOne(); unit.FlushOne(); unit.FlushOne();
}
TEST(DescriptorOperation, OversizedDescriptorClosesBeforeFailing) {
    DescriptorTestUnit unit; unit.descriptor = {0x10, 0x00, 0, 0};
    Legacy::DescriptorAccessor accessor(unit);
    std::optional<ReadResult> result;
    accessor.readUnitIdentifier([&](const auto& value) { result = value; });
    ASSERT_TRUE(result); EXPECT_FALSE(result->success);
    ASSERT_EQ(unit.commands.size(), 3); EXPECT_EQ(unit.commands.back()[4], 0);
}
} // namespace

TEST(OnceCompletion, MoveDisarmsAndAbandonmentCancelsOnce) {
    int calls = 0;
    {
        ASFW::Common::OnceCompletion<int> source([&](int result) { ++calls; EXPECT_EQ(result, -1); }, -1);
        auto target = std::move(source);
    }
    EXPECT_EQ(calls, 1);
}
TEST(OnceCompletion, InvocationConsumesCallback) {
    int calls = 0;
    {
        ASFW::Common::OnceCompletion<int> completion([&](int result) { ++calls; EXPECT_EQ(result, 7); }, -1);
        completion.Invoke(7);
        completion.Invoke(8);
    }
    EXPECT_EQ(calls, 1);
}
