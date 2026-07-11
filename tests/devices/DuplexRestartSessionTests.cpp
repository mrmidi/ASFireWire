#include <gtest/gtest.h>

#include "Audio/Protocols/Duplex/DuplexRestartSession.hpp"

namespace {

using ASFW::Audio::AudioDuplexChannels;
using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::ClassifyRestartReason;
using ASFW::Audio::ClearRestartProgress;
using ASFW::Audio::DuplexClockRequestCompletion;
using ASFW::Audio::DuplexClockRequestOutcome;
using ASFW::Audio::DuplexRestartErrorClass;
using ASFW::Audio::DuplexRestartFailureCause;
using ASFW::Audio::DuplexRestartIssueInfo;
using ASFW::Audio::DuplexRestartPhase;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::DuplexRestartState;
using ASFW::Audio::DuplexRestartSession;

constexpr AudioClockConfig k48kInternal{
    .sampleRateHz = 48000U,
};

constexpr AudioClockConfig k96kInternal{
    .sampleRateHz = 96000U,
};

TEST(DuplexRestartSessionTests, ClassifyRestartReasonReturnsInitialStartWithoutPriorIntent) {
    EXPECT_EQ(ClassifyRestartReason(nullptr, k48kInternal), DuplexRestartReason::kInitialStart);

    DuplexRestartSession emptySession{};
    EXPECT_EQ(ClassifyRestartReason(&emptySession, k48kInternal), DuplexRestartReason::kInitialStart);

    DuplexRestartSession guidOnlySession{
        .guid = 0x130e0402004713ULL,
        .channels = AudioDuplexChannels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        .phase = DuplexRestartPhase::kIdle,
    };
    EXPECT_EQ(ClassifyRestartReason(&guidOnlySession, k48kInternal), DuplexRestartReason::kInitialStart);
}

TEST(DuplexRestartSessionTests, ClassifyRestartReasonDetectsRateChangesBeforeRecovery) {
    DuplexRestartSession prior{
        .guid = 0x130e0402004713ULL,
        .channels = AudioDuplexChannels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        .reason = DuplexRestartReason::kInitialStart,
        .desiredClock = k48kInternal,
        .phase = DuplexRestartPhase::kRunning,
    };

    EXPECT_EQ(ClassifyRestartReason(&prior, k96kInternal), DuplexRestartReason::kSampleRateChange);
}

TEST(DuplexRestartSessionTests, ClassifyRestartReasonReturnsRecoveryForFailedSameConfigRestart) {
    DuplexRestartSession prior{
        .guid = 0x130e0402004713ULL,
        .channels = AudioDuplexChannels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        .reason = DuplexRestartReason::kInitialStart,
        .desiredClock = k48kInternal,
        .phase = DuplexRestartPhase::kFailed,
    };

    EXPECT_EQ(ClassifyRestartReason(&prior, k48kInternal),
              DuplexRestartReason::kRecoverAfterTimingLoss);
}

TEST(DuplexRestartSessionTests, ClearRestartProgressPreservesIntentButClearsExecutionState) {
    DuplexRestartSession session{
        .guid = 0x130e0402004713ULL,
        .channels = AudioDuplexChannels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
        .reason = DuplexRestartReason::kManualReconfigure,
        .desiredClock = k48kInternal,
        .phase = DuplexRestartPhase::kRunning,
        .state = DuplexRestartState::kRunning,
        .lastFailure = DuplexRestartIssueInfo{
            .failedPhase = DuplexRestartPhase::kProgrammingDeviceRx,
            .errorClass = DuplexRestartErrorClass::kStageFailure,
            .cause = DuplexRestartFailureCause::kProgramRx,
            .status = kIOReturnTimeout,
            .retryable = true,
            .rollbackAttempted = true,
            .rollbackStatus = kIOReturnSuccess,
            .hostStateKnown = true,
            .deviceStateKnown = true,
            .restartId = 11,
        },
        .lastInvalidation = DuplexRestartIssueInfo{
            .failedPhase = DuplexRestartPhase::kPreparingDevice,
            .errorClass = DuplexRestartErrorClass::kEpochInvalidated,
            .cause = DuplexRestartFailureCause::kTimingLoss,
            .status = kIOReturnAborted,
            .retryable = true,
            .rollbackAttempted = false,
            .rollbackStatus = kIOReturnSuccess,
            .hostStateKnown = true,
            .deviceStateKnown = true,
            .restartId = 12,
        },
        .lastClockCompletion = DuplexClockRequestCompletion{
            .token = 7,
            .desiredClock = k48kInternal,
            .reason = DuplexRestartReason::kManualReconfigure,
            .outcome = DuplexClockRequestOutcome::kApplied,
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

    ClearRestartProgress(session, DuplexRestartPhase::kIdle);

    EXPECT_EQ(session.guid, 0x130e0402004713ULL);
    EXPECT_EQ(session.reason, DuplexRestartReason::kManualReconfigure);
    EXPECT_EQ(session.desiredClock.sampleRateHz, k48kInternal.sampleRateHz);
    EXPECT_EQ(session.phase, DuplexRestartPhase::kIdle);
    EXPECT_EQ(session.state, DuplexRestartState::kIdle);
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
