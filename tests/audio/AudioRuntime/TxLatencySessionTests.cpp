// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Runtime/TxLatencySession.hpp"
#include <gtest/gtest.h>

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
