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

using ASFW::Protocols::AVC::FCPFrame;
using ASFW::Protocols::AVC::FCPStatus;
using ASFW::Testing::AvcReply;
using ASFW::Testing::AvcTestRig;

// STATUS / unit / UNIT_INFO — the smallest legal AV/C frame.
FCPFrame MakeUnitInfoCommand() {
    FCPFrame command{};
    command.length = 3;
    command.data[0] = 0x00;
    command.data[1] = 0xFF;
    command.data[2] = 0x30;
    return command;
}

// A VENDOR-DEPENDENT frame carrying the Apogee OUI, so the wrong-OUI echo has
// something to corrupt.
FCPFrame MakeVendorCommand() {
    FCPFrame command{};
    command.length = 8;
    command.data[0] = 0x00;  // STATUS
    command.data[1] = 0xFF;  // unit
    command.data[2] = 0x00;  // VENDOR-DEPENDENT
    command.data[3] = 0x00;  // OUI 00:03:db
    command.data[4] = 0x03;
    command.data[5] = 0xDB;
    command.data[6] = 0x50;  // 'P'
    command.data[7] = 0x43;  // 'C'
    return command;
}

struct Capture {
    int count{0};
    FCPStatus status{FCPStatus::kTransportError};
    FCPFrame response{};
};

auto Recorder(Capture& capture) {
    return [&capture](FCPStatus status, const FCPFrame& response) {
        ++capture.count;
        capture.status = status;
        capture.response = response;
    };
}

TEST(AvcTestRig, InitializesTransportAgainstTheDeferredBus) {
    AvcTestRig rig;
    EXPECT_TRUE(rig.IsReady());
    EXPECT_NE(rig.Transport(), nullptr);
}

TEST(AvcTestRig, DefaultReplyAcceptsAndRecordsTheCommand) {
    AvcTestRig rig;
    Capture capture;

    const auto handle = rig.Transport()->SubmitCommand(MakeUnitInfoCommand(), Recorder(capture));
    EXPECT_TRUE(handle.IsValid());
    EXPECT_EQ(capture.count, 0) << "command must not complete before the bus is drained";

    EXPECT_EQ(rig.Drain(), 1U);
    EXPECT_EQ(capture.count, 1);
    EXPECT_EQ(capture.status, FCPStatus::kOk);
    EXPECT_EQ(capture.response.data[0], 0x09);  // ACCEPTED

    ASSERT_EQ(rig.Target().CommandCount(), 1U);
    EXPECT_EQ(rig.Target().Commands()[0].data[2], 0x30);
}

TEST(AvcTestRig, ScriptedRejectionReachesTheCaller) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::Rejected());

    (void)rig.Transport()->SubmitCommand(MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();

    // The transport delivers the frame; the AV/C response code is the caller's
    // to interpret, which is exactly what the Duet's SendVendorCommand does.
    EXPECT_EQ(capture.status, FCPStatus::kOk);
    EXPECT_EQ(capture.response.data[0], 0x0A);  // REJECTED
}

TEST(AvcTestRig, ScriptedReplyTakesPrecedenceOverTheDeviceModel) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().SetDeviceModel(
        [](std::span<const uint8_t>) -> std::optional<AvcReply> { return AvcReply::Accepted(); });
    rig.Target().Script(AvcReply::NotImplemented());

    (void)rig.Transport()->SubmitCommand(MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();

    EXPECT_EQ(capture.response.data[0], 0x08);  // NOT_IMPLEMENTED
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

    (void)rig.Transport()->SubmitCommand(MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();

    EXPECT_EQ(capture.response.data[0], 0x0C);  // IMPLEMENTED/STABLE
}

TEST(AvcTestRig, ShortFrameIsRejectedAndLeavesTheCommandOutstanding) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::ShortFrame());

    (void)rig.Transport()->SubmitCommand(MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();

    // A frame below kAVCFrameMinSize must not be parsed as a response; the
    // command stays pending until its timeout fires.
    EXPECT_EQ(capture.count, 0);

    rig.ExpireFcpTimeout();
    EXPECT_EQ(capture.count, 1);
    EXPECT_EQ(capture.status, FCPStatus::kTimeout);
}

TEST(AvcTestRig, WrongOuiEchoIsDeliveredForTheCallerToReject) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::Accepted().WithWrongOui());

    (void)rig.Transport()->SubmitCommand(MakeVendorCommand(), Recorder(capture));
    rig.Drain();

    ASSERT_EQ(capture.status, FCPStatus::kOk);
    // The transport matches on ctype/subunit/opcode only, so a corrupted OUI
    // surfaces to the protocol layer — which is where OUI checking belongs.
    EXPECT_EQ(capture.response.data[3], 0xFF);
    EXPECT_EQ(capture.response.data[5], 0xFF);
}

TEST(AvcTestRig, SilentTargetTimesOut) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::NoResponse());

    (void)rig.Transport()->SubmitCommand(MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();
    EXPECT_EQ(capture.count, 0);

    rig.ExpireFcpTimeout();
    EXPECT_EQ(capture.count, 1);
    EXPECT_EQ(capture.status, FCPStatus::kTimeout);
}

TEST(AvcTestRig, InterimResponseDefersCompletionUntilTheFinalFrame) {
    AvcTestRig rig;
    Capture capture;
    rig.Target().Script(AvcReply::Interim());

    (void)rig.Transport()->SubmitCommand(MakeUnitInfoCommand(), Recorder(capture));
    rig.Drain();
    EXPECT_EQ(capture.count, 0) << "INTERIM only extends the deadline";

    // The target's final answer arrives on the same pending command.
    rig.Transport()->OnFCPResponse(2, 1, std::vector<uint8_t>{0x09, 0xFF, 0x30});
    EXPECT_EQ(capture.count, 1);
    EXPECT_EQ(capture.status, FCPStatus::kOk);
}

TEST(AvcTestRig, DrainSettlesCommandsChainedFromACompletion) {
    AvcTestRig rig;
    int firstCount = 0;
    int secondCount = 0;

    (void)rig.Transport()->SubmitCommand(
        MakeUnitInfoCommand(), [&](FCPStatus, const FCPFrame&) {
            ++firstCount;
            (void)rig.Transport()->SubmitCommand(
                MakeVendorCommand(),
                [&secondCount](FCPStatus, const FCPFrame&) { ++secondCount; });
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

    (void)rig.Transport()->SubmitCommand(MakeUnitInfoCommand(), Recorder(first));
    (void)rig.Transport()->SubmitCommand(MakeVendorCommand(), Recorder(second));
    rig.Drain();

    ASSERT_EQ(rig.Target().CommandCount(), 2U);
    EXPECT_EQ(rig.Target().Commands()[0].data[2], 0x30);  // UNIT_INFO
    EXPECT_EQ(rig.Target().Commands()[1].data[2], 0x00);  // VENDOR-DEPENDENT
    EXPECT_EQ(first.status, FCPStatus::kOk);
    EXPECT_EQ(second.status, FCPStatus::kOk);
}

} // namespace
