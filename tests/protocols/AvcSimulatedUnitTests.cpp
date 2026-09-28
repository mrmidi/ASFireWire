// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcSimulatedUnitTests.cpp - Tests for SimulatedAvcUnit and RecordingFireWireBus FCP integration.

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Core/IAvcUnit.hpp"
#include "RecordingFireWireBus.hpp"
#include "SimulatedAvcUnit.hpp"

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
    Send(duetUnit_, Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> res) {
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
    Send(duetUnit_, Cmd::SubunitInfoCommand{.page = 0}, [&](Expected<Cmd::SubunitInfo> res) {
        subunitInfo = res;
    });

    ASSERT_TRUE(subunitInfo.has_value());
    ASSERT_TRUE(subunitInfo->has_value());
    EXPECT_EQ((*subunitInfo)->entryCount, 2);
    EXPECT_EQ((*subunitInfo)->entries[0].type, SubunitType::kAudio);
    EXPECT_EQ((*subunitInfo)->entries[1].type, SubunitType::kMusic);

    std::optional<Expected<Cmd::UnitIsochronousExternalPlugs>> plugInfo;
    Send(duetUnit_, Cmd::UnitPlugInfoIsoExtCommand{}, [&](Expected<Cmd::UnitIsochronousExternalPlugs> res) {
        plugInfo = res;
    });

    ASSERT_TRUE(plugInfo.has_value());
    ASSERT_TRUE(plugInfo->has_value());
    EXPECT_EQ((*plugInfo)->isochronousInputs, 1);
    EXPECT_EQ((*plugInfo)->isochronousOutputs, 1);
}

TEST_F(AvcSimulatedUnitTests, Phase88StreamFormatQuery) {
    EXPECT_EQ(phase88Unit_.Guid(), 0x000AAC0300B1D1F7ULL);

    // Query single format for Iso In 0 via 0x2F
    auto cmd = Cmd::BuildStreamFormatSingleStatus(
        Cmd::StreamFormatOpcode::kStreamFormatSupport,
        SubunitAddress::Unit(),
        Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kInput, Cmd::UnitPlugType::kPcr, 0));
    ASSERT_TRUE(cmd.has_value());

    std::optional<Expected<Response>> rawResponse;
    phase88Unit_.Submit(*cmd, phase88Unit_.CurrentGeneration(), [&](Expected<Response> res) {
        rawResponse = res;
    });

    ASSERT_TRUE(rawResponse.has_value());
    ASSERT_TRUE(rawResponse->has_value());
    auto singleFmt = Cmd::ParseStreamFormatSingle(**rawResponse, ResponseCode::kImplementedStable);
    ASSERT_TRUE(singleFmt.has_value());
    EXPECT_EQ(singleFmt->format.compound.rate, StreamFormatRate::k48000);
}

TEST_F(AvcSimulatedUnitTests, FaultKnobs_TimeoutAndInterimAndRejections) {
    // 1. Timeout
    duetUnit_.SetTimeoutNext(true);
    bool called = false;
    Send(duetUnit_, Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo>) {
        called = true;
    });
    EXPECT_FALSE(called); // Dropped

    // 2. Interim deferral
    duetUnit_.SetInterimNext(true);
    std::vector<ResponseCode> codes;
    auto cmd = Cmd::BuildUnitInfoStatus();
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
    Send(duetUnit_, Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> res) {
        rejectedResult = res;
    });
    ASSERT_TRUE(rejectedResult.has_value());
    ASSERT_FALSE(rejectedResult->has_value());
    EXPECT_EQ(rejectedResult->error().kind, AvcErrorKind::kUnexpectedResponse);
    EXPECT_EQ(*rejectedResult->error().response, ResponseCode::kRejected);

    // 4. Not Implemented
    duetUnit_.SetNotImplementedNext(true);
    std::optional<Expected<Cmd::UnitInfo>> notImplResult;
    Send(duetUnit_, Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> res) {
        notImplResult = res;
    });
    ASSERT_TRUE(notImplResult.has_value());
    ASSERT_FALSE(notImplResult->has_value());
    EXPECT_EQ(notImplResult->error().kind, AvcErrorKind::kUnexpectedResponse);
    EXPECT_EQ(*notImplResult->error().response, ResponseCode::kNotImplemented);

    // 5. Bus Reset
    duetUnit_.SetBusResetNext(true);
    std::optional<Expected<Cmd::UnitInfo>> busResetResult;
    Send(duetUnit_, Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> res) {
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
    const uint8_t unitInfoCommand[] = {0x01, 0xFF, 0x30, 0x00};
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
    EXPECT_TRUE(bus_.Trace().Lines()[0].find("W ffff.f0000b00 01ff3000") != std::string::npos);
}

} // namespace ASFW::AVC::Testing
