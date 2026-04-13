#include <gtest/gtest.h>

#include "Protocols/Audio/DICE/Core/DICERestartSession.hpp"

namespace {

using ASFW::Audio::AudioDuplexChannels;
using ASFW::Audio::DICE::ClassifyRestartReason;
using ASFW::Audio::DICE::ClearRestartProgress;
using ASFW::Audio::DICE::DiceClockRequestCompletion;
using ASFW::Audio::DICE::DiceClockRequestOutcome;
using ASFW::Audio::DICE::DiceDesiredClockConfig;
using ASFW::Audio::DICE::DiceRestartErrorClass;
using ASFW::Audio::DICE::DiceRestartFailureCause;
using ASFW::Audio::DICE::DiceRestartIssueInfo;
using ASFW::Audio::DICE::DiceRestartPhase;
using ASFW::Audio::DICE::DiceRestartReason;
using ASFW::Audio::DICE::DiceRestartState;
using ASFW::Audio::DICE::DiceRestartSession;

constexpr DiceDesiredClockConfig k48kInternal{
    .sampleRateHz = 48000U,
    .clockSelect = 0x10010U,
};

constexpr DiceDesiredClockConfig k96kInternal{
    .sampleRateHz = 96000U,
    .clockSelect = 0x20010U,
};

constexpr DiceDesiredClockConfig k48kAdat{
    .sampleRateHz = 48000U,
    .clockSelect = 0x10040U,
};

TEST(DICERestartSessionTests, ClassifyRestartReasonReturnsInitialStartWithoutPriorIntent) {
    EXPECT_EQ(ClassifyRestartReason(nullptr, k48kInternal), DiceRestartReason::kInitialStart);

    DiceRestartSession emptySession{};
    EXPECT_EQ(ClassifyRestartReason(&emptySession, k48kInternal), DiceRestartReason::kInitialStart);

    DiceRestartSession guidOnlySession{
        .guid = 0x130e0402004713ULL,
        .channels = AudioDuplexChannels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        .phase = DiceRestartPhase::kIdle,
    };
    EXPECT_EQ(ClassifyRestartReason(&guidOnlySession, k48kInternal), DiceRestartReason::kInitialStart);
}

TEST(DICERestartSessionTests, ClassifyRestartReasonDetectsClockIntentChangesBeforeRecovery) {
    DiceRestartSession prior{
        .guid = 0x130e0402004713ULL,
        .channels = AudioDuplexChannels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        .reason = DiceRestartReason::kInitialStart,
        .desiredClock = k48kInternal,
        .phase = DiceRestartPhase::kRunning,
    };

    EXPECT_EQ(ClassifyRestartReason(&prior, k96kInternal), DiceRestartReason::kSampleRateChange);
    EXPECT_EQ(ClassifyRestartReason(&prior, k48kAdat), DiceRestartReason::kClockSourceChange);
}

TEST(DICERestartSessionTests, ClassifyRestartReasonReturnsRecoveryForFailedSameConfigRestart) {
    DiceRestartSession prior{
        .guid = 0x130e0402004713ULL,
        .channels = AudioDuplexChannels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        .reason = DiceRestartReason::kInitialStart,
        .desiredClock = k48kInternal,
        .phase = DiceRestartPhase::kFailed,
    };

    EXPECT_EQ(ClassifyRestartReason(&prior, k48kInternal),
              DiceRestartReason::kRecoverAfterTimingLoss);
}

TEST(DICERestartSessionTests, ClearRestartProgressPreservesIntentButClearsExecutionState) {
    DiceRestartSession session{
        .guid = 0x130e0402004713ULL,
        .channels = AudioDuplexChannels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        .reason = DiceRestartReason::kManualReconfigure,
        .desiredClock = k48kInternal,
        .phase = DiceRestartPhase::kRunning,
        .state = DiceRestartState::kRunning,
        .lastFailure = DiceRestartIssueInfo{
            .failedPhase = DiceRestartPhase::kProgrammingDeviceRx,
            .errorClass = DiceRestartErrorClass::kStageFailure,
            .cause = DiceRestartFailureCause::kProgramRx,
            .status = kIOReturnTimeout,
            .retryable = true,
            .rollbackAttempted = true,
            .rollbackStatus = kIOReturnSuccess,
            .hostStateKnown = true,
            .deviceStateKnown = true,
            .restartId = 11,
        },
        .lastInvalidation = DiceRestartIssueInfo{
            .failedPhase = DiceRestartPhase::kPreparingDevice,
            .errorClass = DiceRestartErrorClass::kEpochInvalidated,
            .cause = DiceRestartFailureCause::kTimingLoss,
            .status = kIOReturnAborted,
            .retryable = true,
            .rollbackAttempted = false,
            .rollbackStatus = kIOReturnSuccess,
            .hostStateKnown = true,
            .deviceStateKnown = true,
            .restartId = 12,
        },
        .lastClockCompletion = DiceClockRequestCompletion{
            .token = 7,
            .desiredClock = k48kInternal,
            .reason = DiceRestartReason::kManualReconfigure,
            .outcome = DiceClockRequestOutcome::kApplied,
            .status = kIOReturnSuccess,
            .restartId = 13,
        },
        .ownerClaimed = true,
        .devicePrepared = true,
        .deviceRxProgrammed = true,
        .deviceTxArmed = true,
        .deviceRunning = true,
        .hostDuplexClaimed = true,
        .hostPlaybackReserved = true,
        .hostCaptureReserved = true,
        .hostReceiveStarted = true,
        .hostTransmitStarted = true,
    };

    ClearRestartProgress(session, DiceRestartPhase::kIdle);

    EXPECT_EQ(session.guid, 0x130e0402004713ULL);
    EXPECT_EQ(session.reason, DiceRestartReason::kManualReconfigure);
    EXPECT_EQ(session.desiredClock.sampleRateHz, k48kInternal.sampleRateHz);
    EXPECT_EQ(session.desiredClock.clockSelect, k48kInternal.clockSelect);
    EXPECT_EQ(session.phase, DiceRestartPhase::kIdle);
    EXPECT_EQ(session.state, DiceRestartState::kIdle);
    EXPECT_FALSE(session.ownerClaimed);
    EXPECT_FALSE(session.devicePrepared);
    EXPECT_FALSE(session.deviceRxProgrammed);
    EXPECT_FALSE(session.deviceTxArmed);
    EXPECT_FALSE(session.deviceRunning);
    EXPECT_FALSE(session.hostDuplexClaimed);
    EXPECT_FALSE(session.hostPlaybackReserved);
    EXPECT_FALSE(session.hostCaptureReserved);
    EXPECT_FALSE(session.hostReceiveStarted);
    EXPECT_FALSE(session.hostTransmitStarted);
    EXPECT_TRUE(session.lastFailure.has_value());
    EXPECT_TRUE(session.lastInvalidation.has_value());
    EXPECT_TRUE(session.lastClockCompletion.has_value());
}

} // namespace
