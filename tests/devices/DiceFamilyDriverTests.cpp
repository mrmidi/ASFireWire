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
#include "Audio/Protocols/DICE/Core/DiceClockChangeObserver.hpp"

namespace {

using namespace ASFW::Testing::DICE;

TEST(DiceFamilyDriverTests, NotificationAddressIsTheOneTheOwnerValueNames) {
    EXPECT_TRUE(IsNotificationAddress(kNotificationHandlerOffset));
    // The old software-latch address is no longer ours: nothing names it.
    EXPECT_FALSE(IsNotificationAddress(0x00FF0000D1CCULL));
    EXPECT_FALSE(IsNotificationAddress(0x000100000004ULL));
}

// Two DICE devices on one bus, at nodes 2 and 3 of generation 1.
struct TwoDeviceBus {
    static constexpr uint64_t kGuidA = 0xD1CE00000000000AULL;
    static constexpr uint64_t kGuidB = 0xD1CE00000000000BULL;

    ::ASFW::Discovery::DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceNotificationMailbox mailboxA;
    DiceNotificationMailbox mailboxB;

    TwoDeviceBus() {
        Add(kGuidA, 0x02);
        Add(kGuidB, 0x03);
        router.Register(kGuidA, mailboxA);
        router.Register(kGuidB, mailboxB);
    }

    void Add(uint64_t guid, uint8_t node) {
        ::ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = guid;
        rom.gen = Generation{1};
        rom.nodeId = node;
        (void)registry.UpsertFromROM(rom, ::ASFW::Discovery::LinkPolicy{});
    }
};

TEST(DiceNotificationRouterTests, BitsReachOnlyTheDeviceThatWroteThem) {
    TwoDeviceBus bus;
    EXPECT_EQ(bus.router.Deliver(1, 0xFFC3, NotifyBits::kClockAccepted), TwoDeviceBus::kGuidB);
    // A's clock-accepted wait must not end on B's notification.
    EXPECT_EQ(bus.mailboxA.Consume(), 0U);
    EXPECT_EQ(bus.mailboxB.Consume(), NotifyBits::kClockAccepted);
}

TEST(DiceNotificationRouterTests, UnknownNodesAndOldGenerationsAreDropped) {
    TwoDeviceBus bus;
    struct Seen {
        int calls{0};
    } seen;
    bus.router.SetObserver(&seen, [](void* context, uint64_t, uint32_t) {
        ++static_cast<Seen*>(context)->calls;
    });
    EXPECT_EQ(bus.router.Deliver(1, 0xFFC5, NotifyBits::kClockAccepted), 0U);
    // Node 2 in generation 2 has not been discovered yet.
    EXPECT_EQ(bus.router.Deliver(2, 0xFFC2, NotifyBits::kClockAccepted), 0U);
    EXPECT_EQ(bus.mailboxA.Snapshot(), 0U);
    EXPECT_EQ(bus.mailboxB.Snapshot(), 0U);
    EXPECT_EQ(seen.calls, 0);
    bus.router.ClearObserver(&seen);
}

TEST(DiceNotificationRouterTests, ObserverSeesTheDeviceAndStopsWhenCleared) {
    TwoDeviceBus bus;
    struct Seen {
        uint64_t guid{0};
        uint32_t bits{0};
        int calls{0};
    } seen;
    bus.router.SetObserver(&seen, [](void* context, uint64_t guid, uint32_t bits) {
        auto* s = static_cast<Seen*>(context);
        s->guid = guid;
        s->bits |= bits;
        ++s->calls;
    });
    (void)bus.router.Deliver(1, 0xFFC2, NotifyBits::kLockChange);
    EXPECT_EQ(seen.calls, 1);
    EXPECT_EQ(seen.guid, TwoDeviceBus::kGuidA);
    EXPECT_EQ(seen.bits, NotifyBits::kLockChange);

    bus.router.ClearObserver(&seen);
    (void)bus.router.Deliver(1, 0xFFC2, NotifyBits::kExtStatus);
    EXPECT_EQ(seen.calls, 1);
}

TEST(DiceNotificationRouterTests, UnregisteringAReplacedMailboxKeepsTheReplacement) {
    TwoDeviceBus bus;
    DiceNotificationMailbox replacement;
    bus.router.Register(TwoDeviceBus::kGuidA, replacement);
    bus.router.Unregister(TwoDeviceBus::kGuidA, bus.mailboxA);
    (void)bus.router.Deliver(1, 0xFFC2, NotifyBits::kClockAccepted);
    EXPECT_EQ(replacement.Consume(), NotifyBits::kClockAccepted);
    EXPECT_EQ(bus.mailboxA.Consume(), 0U);
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

    // The reference trace rewrites CLOCK_SELECT during prepare, and so does
    // ASFW: TCAT RestartStreaming and Linux select_clock both write it on every
    // start, and skipping it left a Saffire Pro 24 DSP capturing digital zero
    // (hardware, 2026-10-11). ASFW deviates in two ways:
    //  1. DoAwaitStreamingClockLock adds one extra global-state read after the
    //     clock is confirmed, so streams are never enabled on a still-relocking
    //     clock. That read consumes one extra scripted response, locked at target.
    //  2. The capture opens with a GLOBAL_STATUS read (0x7C) whose value the
    //     kext discards. The bring-up starts with the section table instead,
    //     as TCAT RestartStreaming does (AUDIO_SESSION_REDESIGN.md S3).
    const auto& refRequests = ReferencePhase0ParityFixture::kPrepareExpectedRequests;
    const auto& refResponses = ReferencePhase0ParityFixture::kPrepareResponseSteps;
    std::vector<ExpectedRequest> requests(refRequests.begin() + 1, refRequests.end());
    ASSERT_GT(requests.size(), 7U);
    ASSERT_EQ(requests[5].kind, OpKind::Write); // CLOCK_SELECT, as in the reference
    requests.insert(requests.begin() + 7, requests[6]); // the await-lock global read
    // Responses line up with reads only (a write has none): [5] answers the
    // global read after the write; the await-lock read gets the same answer.
    std::vector<ResponseStep> responses(refResponses.begin() + 1, refResponses.end());
    ASSERT_GT(responses.size(), 5U);
    responses.insert(responses.begin() + 6, responses[5]); // second locked global read

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
    EXPECT_TRUE(rig.controller.IsOwnerHeld());

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

TEST(DiceFamilyDriverTests, StopKeepsTheOwner) {
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
    // Held while the device is present, as TCAT and Linux do.
    EXPECT_TRUE(rig.controller.IsOwnerHeld());
    EXPECT_NE(rig.bus.Owner(), kOwnerNoOwner);
    ExpectOperations(rig.bus.Operations(), ExpectedStopOps());
}

TEST(DiceFamilyDriverTests, SecondPrepareInTheSameGenerationClaimsNothing) {
    DuplexRig rig;
    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = 1,
        .hostToDeviceIsoChannel = 0,
    };
    std::optional<IOReturn> first;
    rig.controller.PrepareDuplex48k(channels, [&first](IOReturn status) { first = status; });
    ASSERT_EQ(first, kIOReturnSuccess);
    ASSERT_EQ(rig.controller.StopDuplex(), kIOReturnSuccess);

    rig.bus.ClearOperations();
    std::optional<IOReturn> second;
    rig.controller.PrepareDuplex48k(channels, [&second](IOReturn status) { second = status; });
    ASSERT_EQ(second, kIOReturnSuccess);
    EXPECT_TRUE(rig.controller.IsOwnerHeld());
    for (const auto& op : rig.bus.Operations()) {
        EXPECT_NE(op.kind, OpKind::Lock) << "claimed again at 0x" << std::hex << op.addressLo;
    }
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
    EXPECT_TRUE(rig.controller.IsOwnerHeld());
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
    EXPECT_TRUE(rig.controller.IsOwnerHeld());

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
    EXPECT_TRUE(rig.controller.IsOwnerHeld());
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
    rig.bus.SetGlobalClockState(
        ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
        48000U,
        NotifyBits::kClockAccepted);
    rig.bus.SetClockSelectWriteHandler([&rig] {
        rig.bus.SetGlobalClockState(
            ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
            48000U,
            NotifyBits::kClockAccepted);
        rig.notifications.Publish(NotifyBits::kClockAccepted);
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
    // The rollback undoes the bring-up, not the ownership.
    EXPECT_TRUE(rig.controller.IsOwnerHeld());
    EXPECT_EQ(rig.timer.NowNs(), 150'000'000ULL);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(DiceFamilyDriverTests, LateClockAcceptedNotifyDoesNotTriggerRollback) {
    // Missing acceptance is tolerated only after the full notification deadline
    // and a final GLOBAL confirmation, rather than trusting pre-write status.
    DuplexRig rig;
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
    EXPECT_EQ(rig.timer.NowNs(), 150'000'000ULL);
}

TEST(DiceFamilyDriverTests, GlobalStateConfirmationRecoversIfMailboxMissesClockAccepted) {
    // A latched GLOBAL notification recovers a lost mailbox write at the deadline.
    DuplexRig rig;
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
    EXPECT_EQ(rig.timer.NowNs(), 150'000'000ULL);
}

// Reproduce power-up timing independently of status-read count: the initial
// target status is stale until the firmware reacts to our same-value write.
TEST(DiceFamilyDriverTests, SameValueClockWriteWaitsForDelayedReconfigurationAndRelock) {
    DuplexRig rig;
    rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, kClockSelect48kInternal);
    rig.bus.SetClockSelectWriteHandler([&rig] {
        (void)rig.timer.ScheduleAfter(90'000'000ULL, [&rig] {
            rig.bus.SetGlobalClockState(ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
                                       48000U);
            rig.bus.Device().RaiseNotification(NotifyBits::kClockAccepted |
                NotifyBits::kRxConfigChange | NotifyBits::kTxConfigChange | NotifyBits::kLockChange);
        });
        (void)rig.timer.ScheduleAfter(290'000'000ULL, [&rig] {
            rig.bus.PublishClockAccepted(NotifyBits::kLockChange);
        });
    });
    const auto result = rig.driver.Prepare({.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        {.sampleRateHz = 48000U, .clockSelect = kClockSelect48kInternal}, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rig.timer.NowNs(), 290'000'000ULL);
    EXPECT_EQ(rig.bus.Enable(), 0U);
    EXPECT_EQ(rig.notifications.Consume(), 0U);
}

TEST(DiceFamilyDriverTests, ConfigChangeBeforeClockAcceptedIsRetainedUntilRelock) {
    DuplexRig rig;
    rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, kClockSelect48kInternal);
    rig.bus.SetClockSelectWriteHandler([&rig] {
        // Config changes can be separate writes, including inside CLOCK_SELECT.
        rig.bus.Device().RaiseNotification(NotifyBits::kRxConfigChange);
        (void)rig.timer.ScheduleAfter(40'000'000ULL, [&rig] {
            rig.bus.Device().RaiseNotification(NotifyBits::kTxConfigChange);
        });
        (void)rig.timer.ScheduleAfter(90'000'000ULL, [&rig] {
            rig.bus.PublishClockAccepted(); // STATUS still holds its pre-write value.
        });
        (void)rig.timer.ScheduleAfter(100'000'000ULL, [&rig] {
            rig.bus.SetGlobalClockState(ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
                                       48000U);
            rig.bus.Device().RaiseNotification(NotifyBits::kLockChange);
        });
        (void)rig.timer.ScheduleAfter(290'000'000ULL, [&rig] {
            rig.bus.PublishClockAccepted(NotifyBits::kLockChange);
        });
    });
    const auto result = rig.driver.Prepare({.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        {.sampleRateHz = 48000U, .clockSelect = kClockSelect48kInternal}, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rig.timer.NowNs(), 290'000'000ULL);
    EXPECT_EQ(rig.notifications.Consume(), 0U);
}

TEST(DiceFamilyDriverTests, ChangedClockSelectAlsoWaitsForAcceptanceDespiteTargetStatus) {
    DuplexRig rig;
    rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect,
        (ClockRateIndex::k44100 << ClockSelectBits::kRateShift) |
            static_cast<uint32_t>(ClockSource::Internal));
    rig.bus.SetClockSelectWriteHandler([&rig] {
        (void)rig.timer.ScheduleAfter(90'000'000ULL, [&rig] { rig.bus.PublishClockAccepted(); });
    });
    const auto result = rig.driver.Prepare({.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        {.sampleRateHz = 48000U, .clockSelect = kClockSelect48kInternal}, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rig.timer.NowNs(), 90'000'000ULL);
}

TEST(DiceFamilyDriverTests, LockChangeDuringGlobalReadCannotConfirmItsOlderLockedStatus) {
    DuplexRig rig;
    rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, kClockSelect48kInternal);
    unsigned globalReads = 0;
    rig.bus.SetGlobalReadCompletedHandler([&rig, &globalReads] {
        if (++globalReads == 3) {
            // The device sampled locked STATUS before dropping lock; its
            // LOCK_CHG write overtakes the pending GLOBAL response.
            rig.bus.SetGlobalClockState(ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
                                       48000U);
            rig.bus.Device().RaiseNotification(NotifyBits::kLockChange);
        }
        if (globalReads == 4) {
            // A new read must follow the poll boundary, not premature success
            // and entry into the separate streaming readiness check.
            EXPECT_GE(rig.timer.NowNs(), 10'000'000ULL);
        }
    });
    rig.bus.SetClockSelectWriteHandler([&rig] {
        rig.bus.PublishClockAccepted(NotifyBits::kClockAccepted | NotifyBits::kRxConfigChange);
        (void)rig.timer.ScheduleAfter(290'000'000ULL, [&rig] {
            rig.bus.PublishClockAccepted(NotifyBits::kLockChange);
        });
    });
    const auto result = rig.driver.Prepare({.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        {.sampleRateHz = 48000U, .clockSelect = kClockSelect48kInternal}, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rig.timer.NowNs(), 290'000'000ULL);
    EXPECT_EQ(rig.notifications.Consume(), 0U);
}

TEST(DiceFamilyDriverTests, ReconfigurationWaitStopsIoOnTeardownOrRouteInvalidation) {
    for (const bool teardown : {false, true}) {
        SCOPED_TRACE(teardown);
        DuplexRig rig;
        size_t operationsAtAbort = 0;
        rig.bus.SetClockSelectWriteHandler([&rig, &operationsAtAbort, teardown] {
            rig.bus.SetGlobalClockState(ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
                                       48000U);
            rig.bus.Device().RaiseNotification(NotifyBits::kClockAccepted | NotifyBits::kRxConfigChange);
            (void)rig.timer.ScheduleAfter(100'000'000ULL, [&rig, &operationsAtAbort, teardown] {
                if (teardown) {
                    rig.cancel.store(true, std::memory_order_release);
                } else {
                    rig.routeState.registry.InvalidateLiveMappingsForBusReset();
                }
                operationsAtAbort = rig.bus.Operations().size();
            });
        });
        const auto result = rig.driver.Prepare(
            {.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
            {.sampleRateHz = 48000U, .clockSelect = kClockSelect48kInternal}, false);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), teardown ? kIOReturnAborted : kIOReturnOffline);
        EXPECT_EQ(rig.timer.NowNs(), 100'000'000ULL);
        EXPECT_EQ(rig.bus.Operations().size(), operationsAtAbort);
        EXPECT_FALSE(rig.driver.IsPrepared());
        EXPECT_EQ(rig.bus.Enable(), 0U);
    }
}

TEST(DiceFamilyDriverTests, MissingLockNotificationUsesBoundedReferenceWait) {
    DuplexRig rig;
    rig.bus.SetClockSelectWriteHandler([&rig] {
        rig.bus.PublishClockAccepted(NotifyBits::kClockAccepted | NotifyBits::kRxConfigChange);
    });
    const auto result = rig.driver.Prepare({.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        {.sampleRateHz = 48000U, .clockSelect = kClockSelect48kInternal}, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(rig.timer.NowNs(), 1'000'000'000ULL);
}

TEST(DiceFamilyDriverTests, ReconfigurationThatNeverRelocksDoesNotPrepareOrEnableStreams) {
    DuplexRig rig;
    rig.bus.SetClockSelectWriteHandler([&rig] {
        rig.bus.SetGlobalClockState(ClockRateIndex::k48000 << StatusBits::kNominalRateShift, 48000U);
        rig.bus.Device().RaiseNotification(NotifyBits::kClockAccepted |
            NotifyBits::kRxConfigChange | NotifyBits::kLockChange);
    });
    const auto result = rig.driver.Prepare({.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        {.sampleRateHz = 48000U, .clockSelect = kClockSelect48kInternal}, false);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), kIOReturnTimeout);
    EXPECT_FALSE(rig.driver.IsPrepared());
    EXPECT_EQ(rig.bus.Enable(), 0U);
    EXPECT_EQ(rig.timer.NowNs(), 3'000'000'000ULL);
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

// --- Observer around a CLOCK_SELECT write ------------------------------------

struct RecordingClockObserver final : ::ASFW::Audio::DICE::DiceClockChangeObserver {
    int before{0};
    int after{0};
    uint32_t rateHz{0};
    bool rateChanged{false};
    void BeforeClockSelectWrite(DiceDeviceIo&) noexcept override { ++before; }
    void AfterClockChange(DiceDeviceIo&, uint32_t rate, bool changed) noexcept override {
        ++after;
        rateHz = rate;
        rateChanged = changed;
    }
};

DiceClockConfiguration Clock(uint32_t hz) {
    uint32_t selection = 0;
    EXPECT_TRUE(::ASFW::Audio::DICE::DiceClockSelectForRate(hz, ClockSource::Internal, selection));
    return {.sampleRateHz = hz, .clockSelect = selection};
}

TEST(DiceFamilyDriverTests, IdleClockChangeCallsTheObserverAroundTheWrite) {
    RecordingClockObserver observer;
    DuplexRig rig(DICEBringupPolicy{.clockChangeObserver = &observer});
    const auto applied = rig.driver.ApplyClock(Clock(44100));
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied->appliedClock.sampleRateHz, 44100U);
    EXPECT_EQ(observer.before, 1);
    EXPECT_EQ(observer.after, 1);
    EXPECT_EQ(observer.rateHz, 44100U);
    EXPECT_TRUE(observer.rateChanged);
}

TEST(DiceFamilyDriverTests, SkippedRedundantIdleClockWriteDoesNotCallTheObserver) {
    RecordingClockObserver observer;
    DuplexRig rig(DICEBringupPolicy{.clockChangeObserver = &observer});
    // Requested and reached after the first apply: the second one skips the write.
    ASSERT_TRUE(rig.driver.ApplyClock(Clock(48000)));
    observer = {};
    ASSERT_TRUE(rig.driver.ApplyClock(Clock(48000)));
    EXPECT_EQ(observer.before, 0);
    EXPECT_EQ(observer.after, 0);
}

} // namespace
