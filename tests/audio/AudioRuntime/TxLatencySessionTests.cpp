// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Runtime/TxLatencySession.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace ASFW::Audio::Runtime;

TEST(TxLatencySessionTests, LifecycleTransitions) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    EXPECT_EQ(session.State(), TxLatencySessionState::Idle);

    // Arm session
    EXPECT_TRUE(session.Arm(1, 1, 48000, 5, 100, 0x1234, 8, 100));
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);

    // Re-arming while capturing must fail
    EXPECT_FALSE(session.Arm(2, 1, 48000, 5, 100, 0x1234, 8, 100));

    // Request stop
    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    // Read header
    TxLatencySessionHeader header{};
    session.ReadHeader(header);
    EXPECT_EQ(header.sessionId, 1U);
    EXPECT_EQ(header.state, TxLatencySessionState::Frozen);
    EXPECT_EQ(header.terminationReason, TxLatencyTerminationReason::UserStopped);

    // Can re-arm from Frozen
    EXPECT_TRUE(session.Arm(2, 1, 48000, 5, 100, 0x5678, 8, 100));
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);
}

TEST(TxLatencySessionTests, QuiescentDrainProtocol) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    EXPECT_TRUE(session.Arm(1, 1, 48000, 5, 100, 0x1234, 8, 100));

    // Request stop transitions to Frozen if no writer is active
    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);
}

TEST(TxLatencySessionTests, PaginationAndWireFormats) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    EXPECT_TRUE(session.Arm(42, 1, 48000, 5, 100, 0x1234, 4, 100));

    // Cannot read records while capturing
    TxLatencyRecord records[10]{};
    EXPECT_EQ(session.ReadRecordsPage(0, 10, records), 0U);

    // Wire page while capturing reports capturing state with 0 samples
    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    EXPECT_TRUE(session.CopyWirePage(0, 32, 42, 100, page));
    EXPECT_EQ(page.header.sessionState, static_cast<uint32_t>(TxLatencySessionState::Capturing));
    EXPECT_EQ(page.header.sessionId, 42U);
    EXPECT_EQ(page.samplesInPage, 0U);

    // Mismatched requested session ID is rejected
    EXPECT_FALSE(session.CopyWirePage(0, 32, 99, 100, page));

    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    // Now frozen, query empty results with correct sessionId
    EXPECT_TRUE(session.CopyWirePage(0, 32, 42, 100, page));
    EXPECT_EQ(page.header.sessionState, static_cast<uint32_t>(TxLatencySessionState::Frozen));
    EXPECT_EQ(page.header.sessionId, 42U);
    EXPECT_EQ(page.samplesInPage, 0U);
    EXPECT_EQ(page.totalPages, 0U);
}

TEST(TxLatencySessionTests, ExpirationWithoutCompletions) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    EXPECT_TRUE(session.Arm(10, 1, 48000, 1, 100, 0x1234, 8, 100));
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);

    // Querying with deadline expired transitions session to Frozen
    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    // Note: session initialized with 1 sec deadline in the future.
    // Let's call CheckExpiration or query:
    session.CheckExpiration(); // In real operation, deadline check runs on PollQuiescence/CopyWirePage
    // If deadline is in future, still Capturing:
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);
}

TEST(TxLatencySessionTests, HandleStreamResetPreservesRecords) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    EXPECT_TRUE(session.Arm(7, 1, 48000, 5, 100, 0x1234, 1, 100));
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);

    // Simulate stream reset
    session.HandleStreamReset();
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    TxLatencySessionHeader header{};
    session.ReadHeader(header);
    EXPECT_EQ(header.sessionId, 7U);
    EXPECT_EQ(header.state, TxLatencySessionState::Frozen);
    EXPECT_EQ(header.terminationReason, TxLatencyTerminationReason::StreamReset);

    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    EXPECT_TRUE(session.CopyWirePage(0, 32, 7, 100, page));
    EXPECT_EQ(page.header.sessionId, 7U);
    EXPECT_EQ(page.header.terminationReason, static_cast<uint32_t>(TxLatencyTerminationReason::StreamReset));
}

TEST(TxLatencySessionTests, WireFormat80ByteSampleOffsets) {
    static_assert(sizeof(ASFW::UserClient::Wire::TxLatencySampleWire) == 80);
    static_assert(sizeof(ASFW::UserClient::Wire::TxLatencySessionWireHeader) == 192);
    static_assert(sizeof(ASFW::UserClient::Wire::TxLatencyResultsPageWire) == 192 + 16 + 32 * 80);
}

TEST(TxLatencySessionTests, ConcurrentFinalizationSerialization) {
    (void)ASFW::Timing::initializeHostTimebase();
    for (int iter = 0; iter < 10; ++iter) {
        TxLatencySession session;
        ASSERT_TRUE(session.Arm(100 + iter, 1, 48000, 10, 100, 0x1234, 1, 100));

        constexpr int kThreadCount = 8;
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);

        for (int i = 0; i < kThreadCount; ++i) {
            threads.emplace_back([&session, i]() {
                if (i % 2 == 0) {
                    session.RequestStop(TxLatencyTerminationReason::UserStopped);
                } else {
                    session.PollQuiescence();
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

        TxLatencySessionHeader header{};
        session.ReadHeader(header);
        EXPECT_EQ(header.sessionId, 100U + static_cast<uint32_t>(iter));
        EXPECT_EQ(header.state, TxLatencySessionState::Frozen);
        EXPECT_EQ(header.terminationReason, TxLatencyTerminationReason::UserStopped);
        EXPECT_GT(header.sessionFrozenHostTicks, 0ULL);
    }
}

TEST(TxLatencySessionTests, WriterHandshakeGenerationProtection) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    ASSERT_TRUE(session.Arm(1, 1, 48000, 10, 100, 0x1234, 1, 100));

    // Stale completion with timestamp older than session start must be rejected immediately
    ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline timeline{};
    std::array<ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), static_cast<uint32_t>(slots.size())));
    slots[0].isData = 1;
    slots[0].framesInPacket = 8;
    slots[0].epoch = 1;
    slots[0].firstAudioFrame = 0;
    slots[0].cycleOrdinal = 0;

    PublicationRangeRing pubRing{};
    pubRing.Record(1, 0, 8, 1000, 2000);

    ASFW::Isoch::IsochTxClockPairSample pair{};
    pair.cycleTimer32 = (0U << 25) | (0U << 12) | 0U;
    pair.hostTimeMid = 100'000'000ULL;
    pair.bracketTicks = 10;

    // A completion from BEFORE session start (e.g. hostNow = 1) is rejected
    session.ObserveCompletion(0, (0U << 25) | (0U << 12) | 0U, 0x11, pair, timeline, pubRing, 1);
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);
    TxLatencySessionHeader header{};
    session.ReadHeader(header);
    EXPECT_EQ(header.sampledCount, 0U);

    // Stop and re-arm to bump generation
    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    ASSERT_TRUE(session.Arm(2, 1, 48000, 10, 100, 0x5678, 1, 100));
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);

    // Old completion stamped with timestamp before session 2's start is also rejected
    session.ObserveCompletion(0, (0U << 25) | (0U << 12) | 0U, 0x11, pair, timeline, pubRing, header.sessionStartHostTicks);
    session.ReadHeader(header);
    EXPECT_EQ(header.sampledCount, 0U);
}

TEST(TxLatencySessionTests, SampleAccountingEquationHoldsWithAgedOut) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    ASSERT_TRUE(session.Arm(99, 1, 48000, 10, 100, 0x1234, 1, 100));

    ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline timeline{};
    std::array<ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), static_cast<uint32_t>(slots.size())));
    slots[0].packetIndex = 0;
    slots[0].isData = 1;
    slots[0].framesInPacket = 8;
    slots[0].epoch = 1;
    slots[0].firstAudioFrame = 1000;
    slots[0].cycleOrdinal = 0;
    slots[0].state.store(ASFW::Protocols::Audio::AMDTP::PacketSlotState::Finalized);

    // Timeline provenance setup: mark image 0 ready with commit generation 1
    timeline.SetImageProvenance(0, 0, 1, 1000, 8, static_cast<uint8_t>(ASFW::Audio::Ports::PcmCopyResult::Ready));

    PublicationRangeRing pubRing{};
    // 1. Matched sample: publication covers [1000, 1008]
    const uint64_t now = mach_absolute_time();
    pubRing.Record(1, 1000, 1008, now - 200'000, now - 100'000);

    ASFW::Isoch::IsochTxClockPairSample pair{};
    pair.cycleTimer32 = (1U << 25) | (100U << 12) | 0U;
    pair.hostTimeMid = now;
    pair.bracketTicks = 10;

    // ack_complete (0x11), image 0
    session.ObserveCompletion(0, (1U << 25) | (100U << 12) | 0U,
                             ASFW::Isoch::PackCompletionMetadata(0, 0, 0x11),
                             pair, timeline, pubRing, now + 1000);

    // 2. Publication Gap/AgedOut sample: publication ring does not cover frame 500000
    slots[1].packetIndex = 1;
    slots[1].isData = 1;
    slots[1].framesInPacket = 8;
    slots[1].epoch = 1;
    slots[1].firstAudioFrame = 500'000;
    slots[1].cycleOrdinal = 1;
    slots[1].state.store(ASFW::Protocols::Audio::AMDTP::PacketSlotState::Finalized);
    timeline.SetImageProvenance(1, 0, 1, 500'000, 8, static_cast<uint8_t>(ASFW::Audio::Ports::PcmCopyResult::Ready));
    session.ObserveCompletion(1, (1U << 25) | (101U << 12) | 0U,
                             ASFW::Isoch::PackCompletionMetadata(0, 0, 0x11),
                             pair, timeline, pubRing, now + 2000);

    // 3. TransmitFailed sample (eventCode = underrun 0x04)
    session.ObserveCompletion(0, (1U << 25) | (102U << 12) | 0U,
                             ASFW::Isoch::PackCompletionMetadata(0, 0, 0x04),
                             pair, timeline, pubRing, now + 3000);

    // Freeze session
    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    ASSERT_TRUE(session.CopyWirePage(0, 32, 99, 1, page));

    // Invariant: samplesCaptured must EXACTLY equal the sum of all reported outcomes!
    const uint32_t outcomeSum = page.header.resolvedCount +
                                page.header.unresolvedCount +
                                page.header.transmitFailedCount +
                                page.header.substitutionCount +
                                page.header.invalidCount;
    EXPECT_EQ(page.header.samplesCaptured, outcomeSum);
    EXPECT_EQ(page.header.samplesCaptured, 3U);
    EXPECT_EQ(page.header.resolvedCount, 1U);
    EXPECT_EQ(page.header.unresolvedCount, 1U);
    EXPECT_EQ(page.header.transmitFailedCount, 1U);
}

TEST(TxLatencySessionTests, SeqlockParityBalancedOnAllExits) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    ASSERT_TRUE(session.Arm(101, 1, 48000, 10, 2, 0x1234, 1, 100));

    // Sequence must start at 0 (even)
    EXPECT_EQ(session.StatusSequence() % 2, 0U);

    ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline timeline{};
    std::array<ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), static_cast<uint32_t>(slots.size())));
    slots[0].packetIndex = 0;
    slots[0].isData = 1;
    slots[0].framesInPacket = 8;
    slots[0].epoch = 1;
    slots[0].firstAudioFrame = 1000;
    slots[0].cycleOrdinal = 0;
    slots[0].state.store(ASFW::Protocols::Audio::AMDTP::PacketSlotState::Finalized);
    timeline.SetImageProvenance(0, 0, 1, 1000, 8, static_cast<uint8_t>(ASFW::Audio::Ports::PcmCopyResult::Ready));

    PublicationRangeRing pubRing{};
    const uint64_t now = mach_absolute_time();
    pubRing.Record(1, 1000, 1008, now - 200'000, now - 100'000);

    ASFW::Isoch::IsochTxClockPairSample pair{};
    pair.cycleTimer32 = (1U << 25) | (100U << 12) | 0U;
    pair.hostTimeMid = now;
    pair.bracketTicks = 10;

    // Observe sample 1: normal matched sample
    session.ObserveCompletion(0, (1U << 25) | (100U << 12) | 0U,
                             ASFW::Isoch::PackCompletionMetadata(0, 0, 0x11),
                             pair, timeline, pubRing, now + 1000);
    EXPECT_EQ(session.StatusSequence() % 2, 0U); // must remain even!

    // Observe sample 2: reaches capacity (budget was 2)
    session.ObserveCompletion(0, (1U << 25) | (101U << 12) | 0U,
                             ASFW::Isoch::PackCompletionMetadata(0, 0, 0x11),
                             pair, timeline, pubRing, now + 2000);
    EXPECT_EQ(session.StatusSequence() % 2, 0U); // must remain even on capacity reached!

    // Observe sample 3: capacity reached early return path
    session.ObserveCompletion(0, (1U << 25) | (102U << 12) | 0U,
                             ASFW::Isoch::PackCompletionMetadata(0, 0, 0x11),
                             pair, timeline, pubRing, now + 3000);
    EXPECT_EQ(session.StatusSequence() % 2, 0U); // must remain even!

    // Header read must successfully read counters (not zeroed due to spin-failure)
    TxLatencySessionHeader header{};
    session.ReadHeader(header);
    EXPECT_EQ(header.sampledCount, 2U);
    EXPECT_EQ(header.matchedCount, 2U);
}

TEST(TxLatencySessionTests, StaleGenerationFinalizerRejected) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    ASSERT_TRUE(session.Arm(1, 1, 48000, 10, 100, 0x1234, 1, 100));
    EXPECT_EQ(session.SessionGeneration(), 1U);

    // Stop and freeze session 1
    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    // Arm session 2 (generation 2)
    ASSERT_TRUE(session.Arm(2, 1, 48000, 10, 100, 0x5678, 1, 100));
    EXPECT_EQ(session.SessionGeneration(), 2U);
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);

    // A stale finalizer thread with target generation 1 attempts to finalize
    const bool finalizedStale = session.TryFinalize(1);
    EXPECT_FALSE(finalizedStale);
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing); // Session 2 must remain Capturing!

    // Session 2 stops and finalizes normally with generation 2
    session.RequestStop(TxLatencyTerminationReason::DeadlineExpired);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    TxLatencySessionHeader header{};
    session.ReadHeader(header);
    EXPECT_EQ(header.sessionId, 2U);
    EXPECT_EQ(header.terminationReason, TxLatencyTerminationReason::DeadlineExpired);
}

TEST(TxLatencySessionTests, LiveHeaderReadsDuringCapturingAndStopRequested) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    ASSERT_TRUE(session.Arm(55, 1, 48000, 10, 100, 0x1234, 1, 100));

    // While capturing, termination reason must report None and frozen ticks must be 0
    TxLatencySessionHeader header{};
    session.ReadHeader(header);
    EXPECT_EQ(header.state, TxLatencySessionState::Capturing);
    EXPECT_EQ(header.terminationReason, TxLatencyTerminationReason::None);
    EXPECT_EQ(header.sessionFrozenHostTicks, 0ULL);

    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    ASSERT_TRUE(session.CopyWirePage(0, 32, 55, 1, page));
    EXPECT_EQ(page.header.sessionState, static_cast<uint32_t>(TxLatencySessionState::Capturing));
    EXPECT_EQ(page.header.terminationReason, static_cast<uint32_t>(TxLatencyTerminationReason::None));
    EXPECT_EQ(page.header.frozenHostTicks, 0ULL);
    EXPECT_EQ(page.samplesInPage, 0U);

    // Freeze session
    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    // Now frozen, termination metadata is visible
    session.ReadHeader(header);
    EXPECT_EQ(header.state, TxLatencySessionState::Frozen);
    EXPECT_EQ(header.terminationReason, TxLatencyTerminationReason::UserStopped);
    EXPECT_GT(header.sessionFrozenHostTicks, 0ULL);

    ASSERT_TRUE(session.CopyWirePage(0, 32, 55, 1, page));
    EXPECT_EQ(page.header.sessionState, static_cast<uint32_t>(TxLatencySessionState::Frozen));
    EXPECT_EQ(page.header.terminationReason, static_cast<uint32_t>(TxLatencyTerminationReason::UserStopped));
    EXPECT_GT(page.header.frozenHostTicks, 0ULL);
}

TEST(TxLatencySessionTests, PausedOldStopDoesNotAffectRearmedSession) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;

    // 1. Arm Session 1 (generation 1, sessionId 10)
    ASSERT_TRUE(session.Arm(10, 1, 48000, 10, 100, 0x1234, 1, 100));
    EXPECT_EQ(session.SessionGeneration(), 1U);
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);

    // 2. Simulate Thread A entering RequestStop while session 1 is capturing.
    // It captures targetGen = 1, but then pauses before executing the CAS.
    const uint32_t pausedOldGen = session.SessionGeneration();
    EXPECT_EQ(pausedOldGen, 1U);

    // 3. While Thread A is paused, another event stops Session 1 normally.
    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    // 4. Session 2 is now armed (generation 2, sessionId 20).
    ASSERT_TRUE(session.Arm(20, 1, 48000, 10, 100, 0x5678, 1, 100));
    EXPECT_EQ(session.SessionGeneration(), 2U);
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);
    EXPECT_EQ(session.SessionId(), 20U);

    // 5. Now Thread A resumes and attempts to execute its stop request with the old generation!
    // In the old implementation, this would blindly CAS state from Capturing to StopRequested.
    // In the atomic lifecycle implementation, the CAS is conditioned on generation 1 and must fail.
    session.RequestStop(TxLatencyTerminationReason::CapacityReached, pausedOldGen);

    // 6. Verify Session 2 was completely unaffected:
    // It must STILL be in Capturing state, not StopRequested or Frozen!
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);
    EXPECT_EQ(session.SessionGeneration(), 2U);
    EXPECT_EQ(session.SessionId(), 20U);

    // 7. Verify Session 2 can continue capturing data.
    ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline timeline{};
    std::array<ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), static_cast<uint32_t>(slots.size())));
    slots[0].packetIndex = 0;
    slots[0].isData = 1;
    slots[0].framesInPacket = 8;
    slots[0].epoch = 1;
    slots[0].firstAudioFrame = 1000;
    slots[0].cycleOrdinal = 0;
    slots[0].state.store(ASFW::Protocols::Audio::AMDTP::PacketSlotState::Finalized);
    timeline.SetImageProvenance(0, 0, 1, 1000, 8, static_cast<uint8_t>(ASFW::Audio::Ports::PcmCopyResult::Ready));

    PublicationRangeRing pubRing{};
    const uint64_t now = mach_absolute_time();
    pubRing.Record(1, 1000, 1008, now - 200'000, now - 100'000);

    ASFW::Isoch::IsochTxClockPairSample pair{};
    pair.cycleTimer32 = (1U << 25) | (100U << 12) | 0U;
    pair.hostTimeMid = now;
    pair.bracketTicks = 10;

    session.ObserveCompletion(0, (1U << 25) | (100U << 12) | 0U,
                             ASFW::Isoch::PackCompletionMetadata(0, 0, 0x11),
                             pair, timeline, pubRing, now + 1000);

    TxLatencySessionHeader header{};
    session.ReadHeader(header);
    EXPECT_EQ(header.sessionId, 20U);
    EXPECT_EQ(header.state, TxLatencySessionState::Capturing);
    EXPECT_EQ(header.sampledCount, 1U);
    EXPECT_EQ(header.terminationReason, TxLatencyTerminationReason::None);

    // 8. Finally stop Session 2 with its own generation and verify clean finalization.
    session.RequestStop(TxLatencyTerminationReason::DeadlineExpired, 2);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    session.ReadHeader(header);
    EXPECT_EQ(header.sessionId, 20U);
    EXPECT_EQ(header.state, TxLatencySessionState::Frozen);
    EXPECT_EQ(header.terminationReason, TxLatencyTerminationReason::DeadlineExpired);
    EXPECT_GT(header.sessionFrozenHostTicks, 0ULL);
}

TEST(TxLatencySessionTests, DelayedPublicationReceiptJoinsAndUpdatesSample) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    ASSERT_TRUE(session.Arm(30, 1, 48000, 10, 100, 0x1234, 1, 100));

    ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline timeline{};
    std::array<ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), static_cast<uint32_t>(slots.size())));
    slots[0].packetIndex = 0;
    slots[0].isData = 1;
    slots[0].framesInPacket = 8;
    slots[0].epoch = 1;
    slots[0].firstAudioFrame = 5000;
    slots[0].cycleOrdinal = 0;
    slots[0].state.store(ASFW::Protocols::Audio::AMDTP::PacketSlotState::Finalized);
    timeline.SetImageProvenance(0, 0, 1, 5000, 8, static_cast<uint8_t>(ASFW::Audio::Ports::PcmCopyResult::Ready));

    // Publication ring is currently EMPTY: coverage for frame 5000 is Pending!
    PublicationRangeRing pubRing{};
    const uint64_t now = mach_absolute_time();

    ASFW::Isoch::IsochTxClockPairSample pair{};
    pair.cycleTimer32 = (1U << 25) | (100U << 12) | 0U;
    pair.hostTimeMid = now;
    pair.bracketTicks = 10;

    session.ObserveCompletion(0, (1U << 25) | (100U << 12) | 0U,
                              ASFW::Isoch::PackCompletionMetadata(0, 0, 0x11),
                              pair, timeline, pubRing, now + 1000);

    TxLatencySessionHeader headerBefore{};
    session.ReadHeader(headerBefore);
    EXPECT_EQ(headerBefore.sampledCount, 1U);
    EXPECT_EQ(headerBefore.matchedCount, 0U);
    EXPECT_EQ(headerBefore.unresolvedCount, 1U);
    EXPECT_EQ(headerBefore.reasonCoveragePending, 1U);

    // Now the publication receipt arrives: [4900, 5100] covers [5000, 5008]
    pubRing.Record(1, 4900, 5100, now - 200'000, now - 100'000);

    // Trigger retry via PollQuiescence(&pubRing)
    session.PollQuiescence(&pubRing);

    TxLatencySessionHeader headerAfter{};
    session.ReadHeader(headerAfter);
    EXPECT_EQ(headerAfter.sampledCount, 1U);
    EXPECT_EQ(headerAfter.matchedCount, 1U);
    EXPECT_EQ(headerAfter.unresolvedCount, 0U);
    EXPECT_EQ(headerAfter.reasonCoveragePending, 0U);

    // Stop session and inspect the sample
    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    ASSERT_TRUE(session.CopyWirePage(0, 32, 30, 1, page));
    EXPECT_EQ(page.samplesInPage, 1U);
    EXPECT_EQ(page.samples[0].outcome, static_cast<uint8_t>(TxLatencyOutcome::Matched));
    EXPECT_EQ(page.samples[0].unresolvedReason, static_cast<uint8_t>(TxLatencyUnresolvedReason::None));
    EXPECT_TRUE(page.samples[0].validityFlags & ASFW::UserClient::Wire::kTxLatencyFlagWaitValid);
    EXPECT_TRUE(page.samples[0].validityFlags & ASFW::UserClient::Wire::kTxLatencyFlagPubValid);
    EXPECT_TRUE(page.samples[0].validityFlags & ASFW::UserClient::Wire::kTxLatencyFlagTxValid);
}

TEST(TxLatencySessionTests, FailedTransmissionDoesNotCorruptCountersOnRetry) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    ASSERT_TRUE(session.Arm(31, 1, 48000, 10, 100, 0x1234, 1, 100));

    ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline timeline{};
    std::array<ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), static_cast<uint32_t>(slots.size())));
    slots[0].packetIndex = 0;
    slots[0].isData = 1;
    slots[0].framesInPacket = 8;
    slots[0].epoch = 1;
    slots[0].firstAudioFrame = 5000;
    slots[0].cycleOrdinal = 0;
    slots[0].state.store(ASFW::Protocols::Audio::AMDTP::PacketSlotState::Finalized);
    timeline.SetImageProvenance(0, 0, 1, 5000, 8, static_cast<uint8_t>(ASFW::Audio::Ports::PcmCopyResult::Ready));

    PublicationRangeRing pubRing{};
    const uint64_t now = mach_absolute_time();

    ASFW::Isoch::IsochTxClockPairSample pair{};
    pair.cycleTimer32 = (1U << 25) | (100U << 12) | 0U;
    pair.hostTimeMid = now;
    pair.bracketTicks = 10;

    // Non-zero event code indicating failure (e.g. 0x02 = evt_underrun, not 0x11 AckComplete)
    session.ObserveCompletion(0, (1U << 25) | (100U << 12) | 0U,
                              ASFW::Isoch::PackCompletionMetadata(0, 0, 0x02),
                              pair, timeline, pubRing, now + 1000);

    TxLatencySessionHeader headerBefore{};
    session.ReadHeader(headerBefore);
    EXPECT_EQ(headerBefore.sampledCount, 1U);
    EXPECT_EQ(headerBefore.matchedCount, 0U);
    EXPECT_EQ(headerBefore.unresolvedCount, 0U);
    EXPECT_EQ(headerBefore.transmitFailedCount, 1U);

    // Provide publication coverage that would cover the frames if it were pending
    pubRing.Record(1, 4900, 5100, now - 200'000, now - 100'000);

    // PollQuiescence must NOT retry failed transmissions or corrupt counters
    session.PollQuiescence(&pubRing);

    TxLatencySessionHeader headerAfter{};
    session.ReadHeader(headerAfter);
    EXPECT_EQ(headerAfter.sampledCount, 1U);
    EXPECT_EQ(headerAfter.matchedCount, 0U);
    EXPECT_EQ(headerAfter.unresolvedCount, 0U);
    EXPECT_EQ(headerAfter.transmitFailedCount, 1U);
    EXPECT_LT(headerAfter.unresolvedCount, 100U); // must not wrap to UINT64_MAX
}

TEST(TxLatencySessionTests, PreservesAssumedDriftAndGeometry) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    constexpr uint32_t kCustomDriftPpm = 300;
    ASSERT_TRUE(session.Arm(32, 1, 48000, 5, 100, 0x1234, 4, kCustomDriftPpm));

    TxLatencySessionHeader header{};
    session.ReadHeader(header);
    EXPECT_EQ(header.assumedDriftPpm, kCustomDriftPpm);

    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    ASSERT_TRUE(session.CopyWirePage(0, 32, 32, 1, page));
    EXPECT_EQ(page.header.assumedDriftPpm, kCustomDriftPpm);
    EXPECT_EQ(page.header.geometryProvenance, (504U << 16) | 1008U);

    auto completed = session.GetCompletedResult();
    ASSERT_NE(completed, nullptr);
    EXPECT_EQ(completed->header.assumedDriftPpm, kCustomDriftPpm);
    EXPECT_EQ(completed->preparationLeadPackets, 1008U);
    EXPECT_EQ(completed->hardwareRingPackets, 504U);
}

TEST(TxLatencySessionTests, LiveHeadersReportCapturedGeometry) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    constexpr uint32_t kLead = 1008;
    constexpr uint32_t kRing = 504;
    ASSERT_TRUE(session.Arm(60, 1, 48000, 5, 100, 0x1234, 4, 100, kLead, kRing));
    EXPECT_EQ(session.State(), TxLatencySessionState::Capturing);

    // Live CopyWirePage during Capturing state must report actual captured geometry, not 6/16
    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    ASSERT_TRUE(session.CopyWirePage(0, 32, 60, 1, page));
    EXPECT_EQ(page.header.sessionState, static_cast<uint32_t>(TxLatencySessionState::Capturing));
    EXPECT_EQ(page.header.geometryProvenance, (kRing << 16) | kLead);
}

TEST(TxLatencySessionTests, ConcurrentAudioAndControlPollingSafety) {
    (void)ASFW::Timing::initializeHostTimebase();
    TxLatencySession session;
    PublicationRangeRing pubRing{};
    ASSERT_TRUE(session.Arm(55, 1, 48000, 10, 100, 0x1234, 1, 100,
                            ASFW::Audio::Shared::AudioTimingGeometry::kTxPreparationLeadPackets,
                            ASFW::Audio::Shared::AudioTimingGeometry::kTxHardwareRingPackets,
                            &pubRing));

    ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline timeline{};
    std::array<ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), static_cast<uint32_t>(slots.size())));
    for (size_t i = 0; i < slots.size(); ++i) {
        slots[i].packetIndex = i;
        slots[i].isData = 1;
        slots[i].framesInPacket = 8;
        slots[i].epoch = 1;
        slots[i].firstAudioFrame = i * 8;
        slots[i].cycleOrdinal = i;
        slots[i].state.store(ASFW::Protocols::Audio::AMDTP::PacketSlotState::Finalized);
        timeline.SetImageProvenance(static_cast<uint32_t>(i), 0, 1, i * 8, 8,
                                    static_cast<uint8_t>(ASFW::Audio::Ports::PcmCopyResult::Ready));
    }

    std::atomic<bool> running{true};
    const uint64_t now = mach_absolute_time();

    // Audio thread: continuously observes completions and calls PollQuiescence(&pubRing)
    std::thread audioThread([&]() {
        ASFW::Isoch::IsochTxClockPairSample pair{};
        pair.cycleTimer32 = (1U << 25) | (100U << 12) | 0U;
        pair.hostTimeMid = now;
        pair.bracketTicks = 10;
        uint64_t pkt = 0;
        while (running.load(std::memory_order_relaxed)) {
            session.ObserveCompletion(pkt % 8, (1U << 25) | (100U << 12) | 0U,
                                      ASFW::Isoch::PackCompletionMetadata(0, 0, 0x11),
                                      pair, timeline, pubRing, now + 1000);
            session.PollQuiescence(&pubRing);
            ++pkt;
        }
    });

    // Control/timer thread: continuously calls PollQuiescence() without pubRing and checks status
    std::thread controlThread([&]() {
        while (running.load(std::memory_order_relaxed)) {
            session.PollQuiescence();
        }
    });

    // Reader thread: continuously reads header and pages wire results
    std::thread readerThread([&]() {
        ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
        TxLatencySessionHeader header{};
        while (running.load(std::memory_order_relaxed)) {
            session.ReadHeader(header);
            (void)session.CopyWirePage(0, 32, 55, 1, page);
        }
    });

    // Run for 50 milliseconds under heavy concurrent contention
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_relaxed);

    audioThread.join();
    controlThread.join();
    readerThread.join();

    // Session can be cleanly stopped
    session.RequestStop(TxLatencyTerminationReason::UserStopped);
    EXPECT_EQ(session.State(), TxLatencySessionState::Frozen);

    TxLatencySessionHeader finalHeader{};
    session.ReadHeader(finalHeader);
    EXPECT_EQ(finalHeader.sessionId, 55U);
    EXPECT_EQ(finalHeader.state, TxLatencySessionState::Frozen);
}


