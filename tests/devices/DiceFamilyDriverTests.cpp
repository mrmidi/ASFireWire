// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DiceFamilyDriverTests.cpp - Parity and state tests for the linear DICE bring-up.
//
// Ported from the DICEDuplexBringupController tests (stage S1): the same
// bodies run against DiceFamilyDriver through DuplexRig's CallbackDriver.
// Tests that advanced time mid-wait now schedule their events on the fake
// timer up front, because the driver waits synchronously.

#include "DICEDuplexTestSupport.hpp"

namespace {

using namespace ASFW::Testing::DICE;

TEST(DiceFamilyDriverTests, NotificationMailboxMatchesReferenceAndLegacyOffsets) {
    EXPECT_TRUE(NotificationMailbox::MatchesDestOffset(NotificationMailbox::kHandlerOffset));
    EXPECT_TRUE(NotificationMailbox::MatchesDestOffset(NotificationMailbox::kLegacyHandlerOffset));
    EXPECT_FALSE(NotificationMailbox::MatchesDestOffset(0x000100000004ULL));
}

TEST(DiceFamilyDriverTests, ProtocolRegisterIOUsesNegotiatedSpeedAndDiceReaderUsesFullGlobalReadSize) {
    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io(bus, bus, routeState.registry, routeState.route);
    DICETransaction tx(io);

    std::optional<AsyncStatus> readStatus;
    (void)io.ReadQuadBE(MakeDICEAddress(kTxSectionOffset + TxOffset::kSize),
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

TEST(DiceFamilyDriverTests, ProtocolRegisterIOReadQuadPropagatesTimeout) {
    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io(bus, bus, routeState.registry, routeState.route);

    static constexpr std::array<ExpectedRequest, 1> kRequests{{
        {OpKind::Read, 0xFFFFU, 0xE00001A8U, 4U, FwSpeed::S400, 0U, {nullptr, 0U}},
    }};
    static constexpr std::array<ResponseStep, 1> kResponses{{
        {OpKind::Read, 0xFFFFU, 0xE00001A8U, 4U, 0U, FwSpeed::S400, AsyncStatus::kTimeout, {nullptr, 0U}},
    }};
    bus.SetScript(kRequests, kResponses);

    std::optional<AsyncStatus> readStatus;
    (void)io.ReadQuadBE(MakeDICEAddress(kTxSectionOffset + TxOffset::kSize),
                  [&readStatus](AsyncStatus status, uint32_t value) {
                      readStatus = status;
                      EXPECT_EQ(value, 0U);
                  });

    ASSERT_TRUE(readStatus.has_value());
    EXPECT_EQ(*readStatus, AsyncStatus::kTimeout);
    EXPECT_TRUE(bus.ScriptConsumed());
}

TEST(DiceFamilyDriverTests, ProtocolRegisterIORejectsInvalidatedRouteBeforeBusAccess) {
    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io(bus, bus, routeState.registry, routeState.route);
    routeState.registry.InvalidateLiveMappingsForBusReset();

    std::optional<AsyncStatus> readStatus;
    (void)io.ReadQuadBE(MakeDICEAddress(kTxSectionOffset + TxOffset::kSize),
                         [&readStatus](AsyncStatus status, uint32_t) { readStatus = status; });

    ASSERT_TRUE(readStatus.has_value());
    EXPECT_EQ(*readStatus, AsyncStatus::kStaleGeneration);
    EXPECT_TRUE(bus.Operations().empty());
}

TEST(DiceFamilyDriverTests, ProtocolRegisterIOReadQuadPropagatesShortRead) {
    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io(bus, bus, routeState.registry, routeState.route);

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
    (void)io.ReadQuadBE(MakeDICEAddress(kTxSectionOffset + TxOffset::kSize),
                  [&readStatus](AsyncStatus status, uint32_t value) {
                      readStatus = status;
                      EXPECT_EQ(value, 0U);
                  });

    ASSERT_TRUE(readStatus.has_value());
    EXPECT_EQ(*readStatus, AsyncStatus::kShortRead);
    EXPECT_TRUE(bus.ScriptConsumed());
}

TEST(DiceFamilyDriverTests, ProtocolRegisterIOWriteQuadUsesNegotiatedSpeedAndBigEndianPayload) {
    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io(bus, bus, routeState.registry, routeState.route);

    std::optional<AsyncStatus> writeStatus;
    (void)io.WriteQuadBE(MakeDICEAddress(kTxSectionOffset + TxOffset::kIsochronous),
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

TEST(DiceFamilyDriverTests, ProtocolRegisterIOCompareSwap64UsesLockAndDecodesBigEndianPayload) {
    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io(bus, bus, routeState.registry, routeState.route);
    const auto ownerOffset = MakeGeneralSections().global.offset + GlobalOffset::kOwnerHi;

    std::optional<AsyncStatus> lockStatus;
    std::optional<uint64_t> previousOwner;
    (void)io.CompareSwap64BE(MakeDICEAddress(ownerOffset),
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

TEST(DiceFamilyDriverTests, PrepareSequenceMatchesReferenceWindow) {
    DuplexRig rig;
    NotificationMailbox::Reset();

    // The reference trace always rewrites CLOCK_SELECT during prepare. ASFW
    // intentionally deviates in two HW-validated ways (see
    // DiceFamilyDriver):
    //  1. The CLOCK_SELECT write is skipped when the pre-claim global read
    //     already reports the target clock (0x020C here) — rewriting it
    //     re-triggers a PLL relock mid-bring-up and fights an idle rate change.
    //  2. DoAwaitStreamingClockLock adds one extra global-state read between
    //     clock-confirm and stream discovery so streams are never enabled on a
    //     still-relocking clock.
    // Net: the Write op is replaced by a second 380-byte global read, and that
    // read consumes one extra scripted response reporting locked-at-target.
    const auto& refRequests = ReferencePhase0ParityFixture::kPrepareExpectedRequests;
    const auto& refResponses = ReferencePhase0ParityFixture::kPrepareResponseSteps;
    std::vector<ExpectedRequest> requests(refRequests.begin(), refRequests.end());
    ASSERT_GT(requests.size(), 7U);
    ASSERT_EQ(requests[6].kind, OpKind::Write); // CLOCK_SELECT in the reference
    requests[6] = requests[7];                  // becomes the await-lock global read
    std::vector<ResponseStep> responses(refResponses.begin(), refResponses.end());
    ASSERT_GT(responses.size(), 6U);
    responses.insert(responses.begin() + 7, responses[6]); // second locked global read

    rig.bus.SetScript(requests, responses);

    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };

    std::optional<IOReturn> startStatus;
    rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });

    ASSERT_TRUE(startStatus.has_value());
    EXPECT_EQ(*startStatus, kIOReturnSuccess);
    EXPECT_TRUE(rig.controller.IsPrepared());
    EXPECT_FALSE(rig.controller.IsArmed());
    EXPECT_FALSE(rig.controller.IsRunning());
    EXPECT_TRUE(rig.controller.IsOwnerClaimed());

    EXPECT_EQ(rig.bus.Owner(), 0xFFC0000100000000ULL);
    ExpectRequests(rig.bus.Operations(), requests);
    EXPECT_TRUE(rig.bus.ScriptConsumed());

    for (const auto& op : rig.bus.Operations()) {
        EXPECT_FALSE(op.addressHi == 0xFFFF && (op.addressLo & 0xFFF00000U) == 0xE0200000U);
    }
}

TEST(DiceFamilyDriverTests, ProgramTxEnableWritesGlobalEnableOnce) {
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

TEST(DiceFamilyDriverTests, ProgramRxMatchesReferenceSegment) {
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

TEST(DiceFamilyDriverTests, StopSequenceReleasesOwnerLast) {
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
    ExpectOperations(rig.bus.Operations(), ExpectedStopOps());
}

TEST(DiceFamilyDriverTests, StopDuplexTeardownCancelAbortsWithoutMoreDeviceIo) {
    DuplexRig rig;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };

    std::optional<IOReturn> startStatus;
    rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });
    ASSERT_TRUE(startStatus.has_value());
    ASSERT_EQ(*startStatus, kIOReturnSuccess);
    ASSERT_TRUE(rig.controller.IsPrepared());

    rig.bus.ClearOperations();
    rig.cancel.store(true, std::memory_order_release);
    const IOReturn stopStatus = rig.controller.StopDuplex();

    EXPECT_EQ(stopStatus, kIOReturnAborted);
    EXPECT_FALSE(rig.controller.IsPrepared());
    EXPECT_FALSE(rig.controller.IsOwnerClaimed());
    EXPECT_TRUE(rig.bus.Operations().empty());
}

TEST(DiceFamilyDriverTests, RestartSessionTracksDevicePhasesAcrossBringupAndStop) {
    DuplexRig rig;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };

    std::optional<IOReturn> prepareStatus;
    std::optional<DuplexPrepareResult> prepareResult;
    rig.controller.PrepareDuplex(
        channels,
        {.sampleRateHz = 48000U, .clockSelect = kClockSelect48kInternal},
        [&prepareStatus, &prepareResult](IOReturn status, DuplexPrepareResult result) {
            prepareStatus = status;
            prepareResult = result;
        });
    ASSERT_TRUE(prepareStatus.has_value());
    ASSERT_EQ(*prepareStatus, kIOReturnSuccess);
    ASSERT_TRUE(prepareResult.has_value());
    EXPECT_EQ(prepareResult->generation.value, 1U);
    EXPECT_EQ(prepareResult->appliedClock.sampleRateHz, 48000U);
    EXPECT_EQ(prepareResult->channels.deviceToHostIsoChannel, channels.deviceToHostIsoChannel);
    EXPECT_EQ(prepareResult->channels.hostToDeviceIsoChannel, channels.hostToDeviceIsoChannel);
    EXPECT_EQ(prepareResult->runtimeCaps.sampleRateHz, 48000U);
    EXPECT_TRUE(rig.controller.IsPrepared());
    EXPECT_FALSE(rig.controller.IsArmed());
    EXPECT_FALSE(rig.controller.IsRunning());
    EXPECT_TRUE(rig.controller.IsOwnerClaimed());

    rig.bus.ClearOperations();
    rig.bus.SetScript(ReferencePhase0ParityFixture::kProgramRxExpectedRequests,
                      ReferencePhase0ParityFixture::kProgramRxResponseSteps);
    std::optional<IOReturn> rxStatus;
    std::optional<DuplexStageResult> rxResult;
    rig.controller.ProgramRx([&rxStatus, &rxResult](IOReturn status, DuplexStageResult result) {
        rxStatus = status;
        rxResult = result;
    });
    ASSERT_TRUE(rxStatus.has_value());
    ASSERT_EQ(*rxStatus, kIOReturnSuccess);
    ASSERT_TRUE(rxResult.has_value());
    EXPECT_EQ(rxResult->phase, DuplexRestartPhase::kDeviceRxProgrammed);
    EXPECT_TRUE(rig.controller.IsPrepared());
    EXPECT_FALSE(rig.controller.IsArmed());

    rig.bus.ClearOperations();
    rig.bus.SetScript(ReferencePhase0ParityFixture::kProgramTxEnableExpectedRequests,
                      ReferencePhase0ParityFixture::kProgramTxEnableResponseSteps);
    std::optional<IOReturn> txStatus;
    std::optional<DuplexStageResult> txResult;
    rig.controller.ProgramTxAndEnableDuplex([&txStatus, &txResult](IOReturn status, DuplexStageResult result) {
        txStatus = status;
        txResult = result;
    });
    ASSERT_TRUE(txStatus.has_value());
    ASSERT_EQ(*txStatus, kIOReturnSuccess);
    ASSERT_TRUE(txResult.has_value());
    EXPECT_EQ(txResult->phase, DuplexRestartPhase::kDeviceTxArmed);
    EXPECT_TRUE(rig.controller.IsArmed());

    rig.bus.ClearScript();
    rig.bus.ClearOperations();
    std::optional<IOReturn> confirmStatus;
    std::optional<DuplexConfirmResult> confirmResult;
    rig.controller.ConfirmDuplexStart(
        [&confirmStatus, &confirmResult](IOReturn status, DuplexConfirmResult result) {
            confirmStatus = status;
            confirmResult = result;
        });
    ASSERT_TRUE(confirmStatus.has_value());
    ASSERT_EQ(*confirmStatus, kIOReturnSuccess);
    ASSERT_TRUE(confirmResult.has_value());
    EXPECT_EQ(confirmResult->runtimeCaps.sampleRateHz, 48000U);
    EXPECT_EQ(confirmResult->appliedClock.sampleRateHz, 48000U);
    EXPECT_TRUE(rig.controller.IsRunning());

    const IOReturn stopStatus = rig.controller.StopDuplex();
    EXPECT_EQ(stopStatus, kIOReturnSuccess);
    EXPECT_FALSE(rig.controller.IsOwnerClaimed());
    EXPECT_FALSE(rig.controller.IsPrepared());
    EXPECT_FALSE(rig.controller.IsArmed());
    EXPECT_FALSE(rig.controller.IsRunning());
}

TEST(DiceFamilyDriverTests,
     ConfirmRejectsDisabledStreamChannelReadback) {
    DuplexRig rig;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };

    std::optional<IOReturn> prepareStatus;
    rig.controller.PrepareDuplex(
        channels,
        {.sampleRateHz = 48000U, .clockSelect = kClockSelect48kInternal},
        [&prepareStatus](IOReturn status, DuplexPrepareResult) {
            prepareStatus = status;
        });
    ASSERT_EQ(prepareStatus, kIOReturnSuccess);

    std::optional<IOReturn> rxStatus;
    rig.controller.ProgramRx(
        [&rxStatus](IOReturn status, DuplexStageResult) {
            rxStatus = status;
        });
    ASSERT_EQ(rxStatus, kIOReturnSuccess);

    std::optional<IOReturn> txStatus;
    rig.controller.ProgramTxAndEnableDuplex(
        [&txStatus](IOReturn status, DuplexStageResult) {
            txStatus = status;
        });
    ASSERT_EQ(txStatus, kIOReturnSuccess);

    rig.bus.SetStreamIsoChannels(0xFFFFFFFFU, 0xFFFFFFFFU);

    std::optional<IOReturn> confirmStatus;
    rig.controller.ConfirmDuplexStart(
        [&confirmStatus](IOReturn status, DuplexConfirmResult) {
            confirmStatus = status;
        });

    ASSERT_TRUE(confirmStatus.has_value());
    EXPECT_EQ(*confirmStatus, kIOReturnNotReady);
    EXPECT_FALSE(rig.controller.IsRunning());
}

TEST(DiceFamilyDriverTests,
     AdvisorySourceLockPreservesTargetRateAndCompletesStart) {
    DuplexRig rig(DICEBringupPolicy{
        .requireSourceLockBeforeStreamEnable = false,
        .requireSourceLockAtConfirm = false,
    });
    NotificationMailbox::Reset();
    rig.bus.SetGlobalClockState(
        ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
        48000U,
        NotifyBits::kClockAccepted);
    rig.bus.SetClockSelectWriteHandler([&rig] {
        rig.bus.SetGlobalClockState(
            ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
            48000U,
            NotifyBits::kClockAccepted);
        NotificationMailbox::Publish(NotifyBits::kClockAccepted);
    });

    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };
    std::optional<IOReturn> prepareStatus;
    rig.controller.PrepareDuplex48k(
        channels, [&prepareStatus](IOReturn status) { prepareStatus = status; });
    ASSERT_EQ(prepareStatus, kIOReturnSuccess);
    ASSERT_TRUE(rig.controller.IsPrepared());

    std::optional<IOReturn> rxStatus;
    rig.controller.ProgramRxForDuplex48k(
        [&rxStatus](IOReturn status) { rxStatus = status; });
    ASSERT_EQ(rxStatus, kIOReturnSuccess);

    std::optional<IOReturn> txStatus;
    rig.controller.ProgramTxAndEnableDuplex48k(
        [&txStatus](IOReturn status) { txStatus = status; });
    ASSERT_EQ(txStatus, kIOReturnSuccess);

    std::optional<IOReturn> confirmStatus;
    rig.controller.ConfirmDuplex48kStart(
        [&confirmStatus](IOReturn status) { confirmStatus = status; });
    EXPECT_EQ(confirmStatus, kIOReturnSuccess);
    EXPECT_TRUE(rig.controller.IsRunning());
}

TEST(DiceFamilyDriverTests,
     ClockAcceptedRetryUsesVirtualTimerAndCompletesAfterDelayedNotification) {
    DuplexRig rig;
    NotificationMailbox::Reset();
    rig.bus.SetGlobalClockState(/*status=*/0, /*sampleRate=*/0);
    rig.bus.SetClockSelectWriteHandler([] {});
    // The device's CLOCK_ACCEPTED arrives 15 ms into the wait: the 0 ms and 10 ms
    // polls miss it, the 20 ms poll consumes it. No wall-clock time passes.
    (void)rig.timer.ScheduleAfter(15'000'000ULL, [&rig] { rig.bus.PublishClockAccepted(); });

    std::optional<IOReturn> startStatus;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };
    rig.controller.PrepareDuplex48k(
        channels, [&startStatus](IOReturn status) { startStatus = status; });

    ASSERT_TRUE(startStatus.has_value());
    EXPECT_EQ(*startStatus, kIOReturnSuccess);
    EXPECT_TRUE(rig.controller.IsPrepared());
    EXPECT_EQ(rig.timer.NowNs(), 20'000'000ULL);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(DiceFamilyDriverTests,
     ClockAcceptedDeadlineTimesOutAfterVirtual150Milliseconds) {
    DuplexRig rig;
    NotificationMailbox::Reset();
    rig.bus.SetGlobalClockState(/*status=*/0, /*sampleRate=*/0);
    rig.bus.SetClockSelectWriteHandler([] {});

    std::optional<IOReturn> startStatus;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };
    rig.controller.PrepareDuplex48k(
        channels, [&startStatus](IOReturn status) { startStatus = status; });

    ASSERT_TRUE(startStatus.has_value());
    EXPECT_EQ(*startStatus, kIOReturnTimeout);
    EXPECT_FALSE(rig.controller.IsPrepared());
    EXPECT_FALSE(rig.controller.IsOwnerClaimed());
    EXPECT_EQ(rig.timer.NowNs(), 150'000'000ULL);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(DiceFamilyDriverTests, LateClockAcceptedNotifyDoesNotTriggerRollback) {
    // With the active clock check, even when the mailbox notification is delayed,
    // the driver reads global state immediately after the clock-select write and
    // short-circuits because the device is already locked at 48 kHz.
    DuplexRig rig;
    NotificationMailbox::Reset();
    rig.bus.SetClockSelectWriteHandler([&rig] {
        (void)rig.timer.ScheduleAfter(3'250'000'000ULL, [&rig] { rig.bus.PublishClockAccepted(); });
    });

    std::optional<IOReturn> startStatus;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };
    rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });

    ASSERT_TRUE(startStatus.has_value());
    EXPECT_EQ(*startStatus, kIOReturnSuccess);
    EXPECT_TRUE(rig.controller.IsPrepared());
    EXPECT_LT(rig.timer.NowNs(), 3'250'000'000ULL) << "completed without waiting for the late notification";
}

TEST(DiceFamilyDriverTests, GlobalStateConfirmationRecoversIfMailboxMissesClockAccepted) {
    // When the mailbox notification never arrives but the device is already locked
    // at 48 kHz, the active clock check after the write short-circuits immediately.
    DuplexRig rig;
    NotificationMailbox::Reset();
    rig.bus.SetClockSelectWriteHandler([&rig]() { rig.bus.LatchClockAccepted(); });

    std::optional<IOReturn> startStatus;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };
    rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });

    ASSERT_TRUE(startStatus.has_value());
    EXPECT_EQ(*startStatus, kIOReturnSuccess);
    EXPECT_TRUE(rig.controller.IsPrepared());
}

TEST(DiceFamilyDriverTests, ProgramTxWritesResolvedLinkSpeedToDiceTxSpeedRegister) {
    // 1. Verify S200 operational link speed (e.g. Midas Venice F24 on Mac):
    // In DICE architecture, TX is from device perspective (device -> Mac capture).
    // The device transmitter must be programmed to transmit at S200 (value 1),
    // never hardcoded/forced to S400 (value 2).
    {
        DuplexRig rig;
        // NodeId 0x02 is the remote DICE device (as configured in RouteState).
        rig.bus.SetSpeed(NodeId{0x02}, FwSpeed::S200);

        const AudioDuplexChannels channels{
            .deviceToHostIsoChannel = 1,
            .hostToDeviceIsoChannel = 0,
        };

        std::optional<IOReturn> startStatus;
        rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });
        ASSERT_TRUE(startStatus.has_value());
        ASSERT_EQ(*startStatus, kIOReturnSuccess);

        std::optional<IOReturn> rxStatus;
        rig.controller.ProgramRxForDuplex48k([&rxStatus](IOReturn status) { rxStatus = status; });
        ASSERT_TRUE(rxStatus.has_value());
        ASSERT_EQ(*rxStatus, kIOReturnSuccess);

        rig.bus.ClearOperations();
        std::optional<IOReturn> txEnableStatus;
        rig.controller.ProgramTxAndEnableDuplex48k([&txEnableStatus](IOReturn status) { txEnableStatus = status; });
        ASSERT_TRUE(txEnableStatus.has_value());
        EXPECT_EQ(*txEnableStatus, kIOReturnSuccess);

        // Find the write to TxOffset::kSpeed (0xE00001B8U)
        const auto& ops = rig.bus.Operations();
        auto it = std::find_if(ops.begin(), ops.end(), [](const RecordedOp& op) {
            return op.kind == OpKind::Write && op.addressLo == 0xE00001B8U;
        });
        ASSERT_NE(it, ops.end()) << "Expected write to TX speed register 0xE00001B8U";
        ASSERT_EQ(it->payload.size(), 4U);
        const uint32_t writtenSpeed = ::ASFW::FW::ReadBE32(it->payload.data());
        EXPECT_EQ(writtenSpeed, 1U) << "Expected TX speed 1 (S200), but found " << writtenSpeed;
        EXPECT_EQ(rig.bus.TxSpeed(), 1U);
        EXPECT_EQ(it->speed, FwSpeed::S200);
    }

    // 2. Verify S400 operational link speed:
    {
        DuplexRig rig;
        rig.bus.SetSpeed(NodeId{0x02}, FwSpeed::S400);

        const AudioDuplexChannels channels{
            .deviceToHostIsoChannel = 1,
            .hostToDeviceIsoChannel = 0,
        };

        std::optional<IOReturn> startStatus;
        rig.controller.PrepareDuplex48k(channels, [&startStatus](IOReturn status) { startStatus = status; });
        ASSERT_TRUE(startStatus.has_value());
        ASSERT_EQ(*startStatus, kIOReturnSuccess);

        std::optional<IOReturn> rxStatus;
        rig.controller.ProgramRxForDuplex48k([&rxStatus](IOReturn status) { rxStatus = status; });
        ASSERT_TRUE(rxStatus.has_value());
        ASSERT_EQ(*rxStatus, kIOReturnSuccess);

        rig.bus.ClearOperations();
        std::optional<IOReturn> txEnableStatus;
        rig.controller.ProgramTxAndEnableDuplex48k([&txEnableStatus](IOReturn status) { txEnableStatus = status; });
        ASSERT_TRUE(txEnableStatus.has_value());
        EXPECT_EQ(*txEnableStatus, kIOReturnSuccess);

        const auto& ops = rig.bus.Operations();
        auto it = std::find_if(ops.begin(), ops.end(), [](const RecordedOp& op) {
            return op.kind == OpKind::Write && op.addressLo == 0xE00001B8U;
        });
        ASSERT_NE(it, ops.end()) << "Expected write to TX speed register 0xE00001B8U";
        ASSERT_EQ(it->payload.size(), 4U);
        const uint32_t writtenSpeed = ::ASFW::FW::ReadBE32(it->payload.data());
        EXPECT_EQ(writtenSpeed, 2U) << "Expected TX speed 2 (S400), but found " << writtenSpeed;
        EXPECT_EQ(rig.bus.TxSpeed(), 2U);
        EXPECT_EQ(it->speed, FwSpeed::S400);
    }
}

} // namespace
