// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcTestRigTests.cpp - Self-coverage for the shared AV/C test rig (FW-138).
//
// These prove the fixture can reach the error paths the OXFW/Apogee
// decomposition tickets need: rejection, malformed frames, wrong-OUI echoes,
// and a silent target. Everything runs against the real FCPTransport.

#include <gtest/gtest.h>

#include "AvcTestRig.hpp"

#include <vector>

namespace {

using ASFW::AVC::AvcErrorKind;
using ASFW::Testing::AvcReply;
using ASFW::Testing::AvcTestRig;
using Reply = ASFW::AVC::Expected<ASFW::AVC::Response>;

// CONTROL / unit / UNIT_INFO — the smallest legal AV/C frame.
ASFW::AVC::CommandFrame MakeUnitInfoCommand() {
    return *ASFW::AVC::CommandFrame::Make(ASFW::AVC::CommandType::kControl, ASFW::AVC::SubunitAddress::Unit(),
                                          ASFW::AVC::Opcode::kUnitInfo, {});
}

// A VENDOR-DEPENDENT frame carrying the Apogee OUI, so the wrong-OUI echo has
// something to corrupt.
ASFW::AVC::CommandFrame MakeVendorCommand() {
    constexpr uint8_t kOperands[] = {0x00, 0x03, 0xDB, 0x50, 0x43};  // OUI 00:03:db, 'P' 'C'
    return *ASFW::AVC::CommandFrame::Make(ASFW::AVC::CommandType::kControl, ASFW::AVC::SubunitAddress::Unit(),
                                          ASFW::AVC::Opcode::kVendorDependent, kOperands);
}

struct Capture {
    int count{0};
    /// nullopt: a response arrived; otherwise the engine's error.
    std::optional<AvcErrorKind> error{AvcErrorKind::kTransportError};
    /// The response frame: code, address, opcode, operands.
    std::vector<uint8_t> response;
};

auto Recorder(Capture& capture) {
    return [&capture](Reply reply) {
        ++capture.count;
        capture.error = reply ? std::nullopt : std::optional<AvcErrorKind>{reply.error().kind};
        capture.response.clear();
        if (reply) {
            capture.response = {static_cast<uint8_t>(reply->code), reply->address.Byte(),
                                static_cast<uint8_t>(reply->opcode)};
            capture.response.insert(capture.response.end(), reply->operands.begin(), reply->operands.end());
        }
    };
}

template <typename Callback>
void Send(AvcTestRig& rig, const ASFW::AVC::CommandFrame& frame, Callback&& completion) {
    rig.Transport()->Submit(frame, rig.Route().generation, std::forward<Callback>(completion));
}

TEST(AvcTestRig, InitializesTransportAgainstTheDeferredBus) {
    AvcTestRig rig;
    EXPECT_TRUE(rig.IsReady());
    EXPECT_NE(rig.Transport(), nullptr);
}

TEST(AvcTestRig, DefaultReplyAcceptsAndRecordsTheCommand) {
    AvcTestRig rig;
    Capture capture;

    Send(rig, MakeUnitInfoCommand(), Recorder(capture));
    EXPECT_EQ(capture.count, 0) << "command must not complete before the bus is drained";

    EXPECT_EQ(rig.Drain(), 1U);
    EXPECT_EQ(capture.count, 1);
    EXPECT_EQ(capture.error, std::nullopt);
    EXPECT_EQ(capture.response[0], 0x09);  // ACCEPTED

    ASSERT_EQ(rig.Target().CommandCount(), 1U);
    EXPECT_EQ(rig.Target().Commands()[0].data[2], 0x30);
}

TEST(AvcTestRig, ScriptedRejectionReachesTheCaller) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::Rejected());

    Send(rig, MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();

    // The transport delivers the frame; the AV/C response code is the caller's
    // to interpret, which is exactly what the Duet's SendVendorCommand does.
    EXPECT_EQ(capture.error, std::nullopt);
    EXPECT_EQ(capture.response[0], 0x0A);  // REJECTED
}

TEST(AvcTestRig, ScriptedReplyTakesPrecedenceOverTheDeviceModel) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().SetDeviceModel(
        [](std::span<const uint8_t>) -> std::optional<AvcReply> { return AvcReply::Accepted(); });
    rig.Target().Script(AvcReply::NotImplemented());

    Send(rig, MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();

    EXPECT_EQ(capture.response[0], 0x08);  // NOT_IMPLEMENTED
    EXPECT_EQ(rig.Target().ScriptedRemaining(), 0U);
}

TEST(AvcTestRig, DeviceModelAnswersWhenNothingIsScripted) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().SetDeviceModel([](std::span<const uint8_t> command) -> std::optional<AvcReply> {
        if (command.size() >= 3 && command[2] == 0x30) {
            return AvcReply::ImplementedStable();
        }
        return std::nullopt;
    });

    Send(rig, MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();

    EXPECT_EQ(capture.response[0], 0x0C);  // IMPLEMENTED/STABLE
}

TEST(AvcTestRig, ShortFrameIsRejectedAndLeavesTheCommandOutstanding) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::ShortFrame());

    Send(rig, MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();

    // A frame below kAVCFrameMinSize must not be parsed as a response; the
    // command stays pending until its timeout fires.
    EXPECT_EQ(capture.count, 0);

    rig.ExpireFcpTimeout();
    EXPECT_EQ(capture.count, 1);
    EXPECT_EQ(capture.error, AvcErrorKind::kTimeout);
}

TEST(AvcTestRig, WrongOuiEchoIsDeliveredForTheCallerToReject) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::Accepted().WithWrongOui());

    Send(rig, MakeVendorCommand(), Recorder(capture));
    rig.Drain();

    ASSERT_EQ(capture.error, std::nullopt);
    // The transport matches on ctype/subunit/opcode only, so a corrupted OUI
    // surfaces to the protocol layer — which is where OUI checking belongs.
    EXPECT_EQ(capture.response[3], 0xFF);
    EXPECT_EQ(capture.response[5], 0xFF);
}

TEST(AvcTestRig, SilentTargetTimesOut) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::NoResponse());

    Send(rig, MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();
    EXPECT_EQ(capture.count, 0);

    rig.ExpireFcpTimeout();
    EXPECT_EQ(capture.count, 1);
    EXPECT_EQ(capture.error, AvcErrorKind::kTimeout);
}

TEST(AvcTestRig, InterimResponseDefersCompletionUntilTheFinalFrame) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::Interim());

    Send(rig, MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();
    EXPECT_EQ(capture.count, 0) << "INTERIM only extends the deadline";

    // The target's final answer arrives on the same pending command.
    rig.Transport()->OnFCPResponse(2, 1, std::vector<uint8_t>{0x09, 0xFF, 0x30});
    rig.Drain();
    EXPECT_EQ(capture.count, 1);
    EXPECT_EQ(capture.error, std::nullopt);
}

TEST(AvcTestRig, DrainSettlesCommandsChainedFromACompletion) {
    AvcTestRig rig;
    int firstCount = 0;
    int secondCount = 0;

    Send(rig, MakeUnitInfoCommand(), [&](Reply) {
        ++firstCount;
        Send(rig, MakeVendorCommand(), [&secondCount](Reply) { ++secondCount; });
    });

    // One Drain() call must settle both hops of the chain.
    EXPECT_EQ(rig.Drain(), 2U);
    EXPECT_EQ(firstCount, 1);
    EXPECT_EQ(secondCount, 1);
    EXPECT_EQ(rig.Target().CommandCount(), 2U);
}

TEST(AvcTestRig, RecordsCommandFramesInSubmissionOrder) {
    AvcTestRig rig;
    Capture first;
    Capture second;

    Send(rig, MakeUnitInfoCommand(), Recorder(first));
    Send(rig, MakeVendorCommand(), Recorder(second));
    rig.Drain();

    ASSERT_EQ(rig.Target().CommandCount(), 2U);
    EXPECT_EQ(rig.Target().Commands()[0].data[2], 0x30);  // UNIT_INFO
    EXPECT_EQ(rig.Target().Commands()[1].data[2], 0x00);  // VENDOR-DEPENDENT
    EXPECT_EQ(first.error, std::nullopt);
    EXPECT_EQ(second.error, std::nullopt);
}

} // namespace
