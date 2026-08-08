// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeTransportTests.cpp - Duet FCP dispatch and meter register reads (FW-129).
//
// The two transports are tested separately, which is the point of the split:
// VendorFcp goes through the real FCPTransport via the shared rig, while
// MeterRegisters never touches AV/C at all.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeTransport.hpp"

#include "AvcTestRig.hpp"

#include <optional>
#include <vector>

namespace {

using ASFW::Audio::Oxford::Apogee::ApogeeVendorCommand;
using ASFW::Audio::Oxford::Apogee::InputMeterState;
using ASFW::Audio::Oxford::Apogee::MixerMeterState;
using ASFW::Testing::AvcReply;
using ASFW::Testing::AvcTestRig;
using Code = ApogeeVendorCommand::Code;
namespace MeterRegisters = ASFW::Audio::Oxford::Apogee::MeterRegisters;
namespace VendorFcp = ASFW::Audio::Oxford::Apogee::VendorFcp;
namespace Apogee = ASFW::Audio::Oxford::Apogee;

/// Resolves through the rig's registry on every call, the way the driver does.
[[nodiscard]] Apogee::RouteProvider LiveRoute(AvcTestRig& rig) {
    return [&rig, guid = rig.Route().guid] { return rig.Routes().CurrentRoute(guid); };
}

[[nodiscard]] Apogee::RouteProvider NoRoute() {
    return [] { return std::optional<ASFW::Discovery::DeviceRouteToken>{}; };
}

// ============================================================================
// Transport B - meter registers (no AV/C involved)
// ============================================================================

TEST(ApogeeMeterRegisters, AddressesSplitAcrossTheQuadletBoundary) {
    // Base 0xFFFFF0080000, input +0x0004, mixer +0x0404 (apogee.rs:128-130).
    constexpr auto input = MeterRegisters::InputAddress();
    constexpr auto mixer = MeterRegisters::MixerAddress();
    static_assert(input.addressHi == 0xFFFFU);
    static_assert(input.addressLo == 0xF0080004U);
    static_assert(mixer.addressHi == 0xFFFFU);
    static_assert(mixer.addressLo == 0xF0080404U);
    EXPECT_NE(input.addressLo, mixer.addressLo);
}

TEST(ApogeeMeterRegisters, DecodesInputLevelsBigEndian) {
    const std::vector<uint8_t> payload{0x00, 0x00, 0x01, 0x00,   // 0x100 = one step
                                       0x7F, 0xFF, 0xFF, 0xFF};  // i32::MAX
    InputMeterState state{};
    ASSERT_TRUE(MeterRegisters::DecodeInput(payload, state));
    EXPECT_EQ(state.levels[0], 0x00000100);
    EXPECT_EQ(state.levels[1], 0x7FFFFFFF);
    EXPECT_EQ(state.levels[1], InputMeterState::kMax);
}

TEST(ApogeeMeterRegisters, DecodesMixerStreamInputsThenMixerOutputs) {
    const std::vector<uint8_t> payload{
        0x00, 0x00, 0x00, 0x01,  // stream-in 0
        0x00, 0x00, 0x00, 0x02,  // stream-in 1
        0x00, 0x00, 0x00, 0x03,  // mixer-out 0
        0x00, 0x00, 0x00, 0x04,  // mixer-out 1
    };
    MixerMeterState state{};
    ASSERT_TRUE(MeterRegisters::DecodeMixer(payload, state));
    EXPECT_EQ(state.streamInputs[0], 1);
    EXPECT_EQ(state.streamInputs[1], 2);
    EXPECT_EQ(state.mixerOutputs[0], 3);
    EXPECT_EQ(state.mixerOutputs[1], 4);
}

TEST(ApogeeMeterRegisters, RejectsShortBlocksWithoutTouchingTheOutput) {
    InputMeterState input{};
    input.levels[0] = 0x1234;
    EXPECT_FALSE(MeterRegisters::DecodeInput(std::vector<uint8_t>(7, 0xFF), input));
    EXPECT_EQ(input.levels[0], 0x1234) << "a rejected decode must not partially write";

    MixerMeterState mixer{};
    EXPECT_FALSE(MeterRegisters::DecodeMixer(std::vector<uint8_t>(15, 0xFF), mixer));
    // An 8-byte read is enough for the input block but not the mixer block.
    EXPECT_FALSE(MeterRegisters::DecodeMixer(std::vector<uint8_t>(8, 0xFF), mixer));
}

TEST(ApogeeMeterRegisters, ReadsInputMetersOffTheBusWithoutAnyFcpTraffic) {
    AvcTestRig rig;
    rig.Bus().MapRead(
        (static_cast<uint64_t>(MeterRegisters::InputAddress().addressHi) << 32U) |
            MeterRegisters::InputAddress().addressLo,
        std::vector<uint8_t>{0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00});

    IOReturn status = kIOReturnNotReady;
    InputMeterState state{};
    MeterRegisters::ReadInput(rig.Bus(), LiveRoute(rig),
                              [&](IOReturn s, InputMeterState value) {
                                  status = s;
                                  state = value;
                              });

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(state.levels[0], 0x200);
    EXPECT_EQ(state.levels[1], 0x300);
    EXPECT_EQ(rig.Target().CommandCount(), 0U) << "meters are not an AV/C transport";
}

TEST(ApogeeMeterRegisters, DeviceWithNoLiveRouteIsRefusedBeforeAnyBusTraffic) {
    AvcTestRig rig;
    IOReturn status = kIOReturnSuccess;
    MeterRegisters::ReadMixer(rig.Bus(), NoRoute(),
                              [&](IOReturn s, MixerMeterState) { status = s; });

    EXPECT_EQ(status, kIOReturnNotReady);
    EXPECT_EQ(rig.Bus().ReadCount(), 0U);
}

TEST(ApogeeMeterRegisters, UnmappedReadFailsRatherThanReportingZeroLevels) {
    AvcTestRig rig;  // nothing mapped, so the bus answers kTimeout
    IOReturn status = kIOReturnSuccess;
    MeterRegisters::ReadInput(rig.Bus(), LiveRoute(rig),
                              [&](IOReturn s, InputMeterState) { status = s; });
    EXPECT_EQ(status, kIOReturnError);
}

TEST(ApogeeMeterRegisters, RouteReboundBeforeTheReadIsResolvedFreshNotFromASnapshot) {
    // FW-142 regression, meter half: meters shared the CSR reads' validator and
    // therefore the same stale-snapshot defect.
    AvcTestRig rig;
    rig.Bus().MapRead(
        (static_cast<uint64_t>(MeterRegisters::InputAddress().addressHi) << 32U) |
            MeterRegisters::InputAddress().addressLo,
        std::vector<uint8_t>{0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00});

    const auto snapshot = rig.Route();
    ASSERT_TRUE(static_cast<bool>(snapshot));

    // Same node, same generation, new routeEpoch.
    ASFW::Discovery::ConfigROM rom{};
    rom.bib.guid = snapshot.guid;
    rom.gen = snapshot.generation;
    rom.nodeId = static_cast<uint8_t>(snapshot.nodeId);
    rig.Routes().InvalidateLiveMappingsForBusReset();
    rig.Routes().UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});

    ASSERT_NE(rig.Route().routeEpoch, snapshot.routeEpoch);
    EXPECT_FALSE(rig.Routes().IsCurrent(snapshot));

    IOReturn status = kIOReturnNotReady;
    InputMeterState state{};
    MeterRegisters::ReadInput(rig.Bus(), LiveRoute(rig),
                              [&](IOReturn s, InputMeterState value) {
                                  status = s;
                                  state = value;
                              });

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(state.levels[0], 0x200);
}

// ============================================================================
// Transport A - AV/C vendor command dispatch
// ============================================================================

TEST(ApogeeVendorFcp, SendsAControlCommandAndReportsSuccess) {
    AvcTestRig rig;
    IOReturn status = kIOReturnNotReady;
    VendorFcp::Send(rig.Transport(), ApogeeVendorCommand::OutVolume(40), /*isStatus=*/false,
                    [&](IOReturn s, const ApogeeVendorCommand&) { status = s; });
    rig.Drain();

    EXPECT_EQ(status, kIOReturnSuccess);
    ASSERT_EQ(rig.Target().CommandCount(), 1U);
    // ctype CONTROL, unit subunit, VENDOR-DEPENDENT opcode.
    EXPECT_EQ(rig.Target().Commands()[0].data[0], 0x00);
    EXPECT_EQ(rig.Target().Commands()[0].data[1], 0xFF);
    EXPECT_EQ(rig.Target().Commands()[0].data[2], 0x00);
}

TEST(ApogeeVendorFcp, PadsFramesToQuadletAlignment) {
    AvcTestRig rig;
    VendorFcp::Send(rig.Transport(), ApogeeVendorCommand::OutVolume(40), /*isStatus=*/false,
                    [](IOReturn, const ApogeeVendorCommand&) {});
    rig.Drain();

    ASSERT_EQ(rig.Target().CommandCount(), 1U);
    // 3 AV/C header + 9 operand base + 1 value = 13, padded up to 16.
    const size_t length = rig.Target().Commands()[0].length;
    EXPECT_EQ(length % 4U, 0U);
    EXPECT_EQ(length, 16U);
    EXPECT_GE(length, ASFW::Protocols::AVC::kAVCFrameMinSize);
    EXPECT_LE(length, ASFW::Protocols::AVC::kAVCFrameMaxSize);
}

TEST(ApogeeVendorFcp, StatusQueryOmitsTheValueOperand) {
    AvcTestRig rig;
    VendorFcp::Send(rig.Transport(), ApogeeVendorCommand::OutVolume(40), /*isStatus=*/true,
                    [](IOReturn, const ApogeeVendorCommand&) {});
    rig.Drain();

    ASSERT_EQ(rig.Target().CommandCount(), 1U);
    EXPECT_EQ(rig.Target().Commands()[0].data[0], 0x01) << "ctype STATUS";
    // 3 + 9 = 12, already quadlet-aligned, so no value byte was appended.
    EXPECT_EQ(rig.Target().Commands()[0].length, 12U);
}

TEST(ApogeeVendorFcp, MissingTransportIsNotReadyRatherThanACrash) {
    IOReturn status = kIOReturnSuccess;
    VendorFcp::Send(nullptr, ApogeeVendorCommand::OutVolume(1), false,
                    [&](IOReturn s, const ApogeeVendorCommand&) { status = s; });
    EXPECT_EQ(status, kIOReturnNotReady);
}

TEST(ApogeeVendorFcp, RejectedCommandSurfacesAsAnError) {
    AvcTestRig rig;
    rig.Target().Script(AvcReply::Rejected());

    IOReturn status = kIOReturnSuccess;
    VendorFcp::Send(rig.Transport(), ApogeeVendorCommand::OutVolume(1), false,
                    [&](IOReturn s, const ApogeeVendorCommand&) { status = s; });
    rig.Drain();
    EXPECT_NE(status, kIOReturnSuccess);
}

TEST(ApogeeVendorFcp, SequenceRunsInOrderAndReturnsEveryResponse) {
    AvcTestRig rig;
    const std::vector<ApogeeVendorCommand> commands{
        ApogeeVendorCommand::Make(Code::OutMute),
        ApogeeVendorCommand::OutVolume(20),
        ApogeeVendorCommand::Make(Code::OutSourceIsMixer),
    };

    IOReturn status = kIOReturnNotReady;
    size_t responseCount = 0;
    VendorFcp::ExecuteSequence(rig.Transport(), commands, /*isStatus=*/false,
                               [&](IOReturn s, const std::vector<ApogeeVendorCommand>& r) {
                                   status = s;
                                   responseCount = r.size();
                               });
    rig.Drain();

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(responseCount, 3U);
    ASSERT_EQ(rig.Target().CommandCount(), 3U);
    EXPECT_EQ(rig.Target().Commands()[0].data[9], static_cast<uint8_t>(Code::OutMute));
    EXPECT_EQ(rig.Target().Commands()[1].data[9], static_cast<uint8_t>(Code::OutVolume));
    EXPECT_EQ(rig.Target().Commands()[2].data[9], static_cast<uint8_t>(Code::OutSourceIsMixer));
}

TEST(ApogeeVendorFcp, SequenceAbortsAtTheFirstFailure) {
    AvcTestRig rig;
    rig.Target().Script(AvcReply::Accepted());
    rig.Target().Script(AvcReply::Rejected());  // second command fails

    const std::vector<ApogeeVendorCommand> commands{
        ApogeeVendorCommand::Make(Code::OutMute),
        ApogeeVendorCommand::OutVolume(20),
        ApogeeVendorCommand::Make(Code::OutSourceIsMixer),
    };

    IOReturn status = kIOReturnSuccess;
    size_t responseCount = 99;
    VendorFcp::ExecuteSequence(rig.Transport(), commands, false,
                               [&](IOReturn s, const std::vector<ApogeeVendorCommand>& r) {
                                   status = s;
                                   responseCount = r.size();
                               });
    rig.Drain();

    EXPECT_NE(status, kIOReturnSuccess);
    // A half-applied params group is worse than a failed one: the third
    // command must never reach the wire.
    EXPECT_EQ(rig.Target().CommandCount(), 2U);
    EXPECT_EQ(responseCount, 0U) << "no partial response set on failure";
}

TEST(ApogeeVendorFcp, EmptySequenceCompletesImmediately) {
    AvcTestRig rig;
    IOReturn status = kIOReturnNotReady;
    VendorFcp::ExecuteSequence(rig.Transport(), {}, false,
                               [&](IOReturn s, const std::vector<ApogeeVendorCommand>&) {
                                   status = s;
                               });
    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(rig.Target().CommandCount(), 0U);
}

} // namespace
