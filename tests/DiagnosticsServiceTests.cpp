//
//  DiagnosticsServiceTests.cpp
//  ASFWTests
//
//  Created by ASFireWire Project on 29.05.2026.
//

#include <gtest/gtest.h>
#include "ASFWDiagnosticsABI.h"
#include <cstddef>

// Static assert compile-time checks to ensure structural alignment and size invariants.
// This is critical since these structures are shared directly with Swift via the C ABI bridging.

static_assert(sizeof(ASFWDiagHeader) == 32, "ASFWDiagHeader size mismatch");
static_assert(offsetof(ASFWDiagHeader, abiVersion) == 0, "abiVersion offset mismatch");
static_assert(offsetof(ASFWDiagHeader, structSize) == 4, "structSize offset mismatch");
static_assert(offsetof(ASFWDiagHeader, status) == 8, "status offset mismatch");
static_assert(offsetof(ASFWDiagHeader, timestampNs) == 16, "timestampNs offset mismatch");
static_assert(offsetof(ASFWDiagHeader, generation) == 24, "generation offset mismatch");
static_assert(offsetof(ASFWDiagHeader, snapshotSeq) == 28, "snapshotSeq offset mismatch");

static_assert(sizeof(ASFWDiagBusContract) == 96, "ASFWDiagBusContract size mismatch");
static_assert(offsetof(ASFWDiagBusContract, header) == 0, "header offset mismatch");
static_assert(offsetof(ASFWDiagBusContract, busId) == 32, "busId offset mismatch");

static_assert(sizeof(ASFWDiagNode) == 164, "ASFWDiagNode size mismatch");
static_assert(offsetof(ASFWDiagNode, ports) == 32, "ports offset mismatch");

static_assert(sizeof(ASFWDiagTopology) == 11008, "ASFWDiagTopology size mismatch");
static_assert(offsetof(ASFWDiagTopology, rawSelfIds) == 32, "rawSelfIds offset mismatch");
static_assert(offsetof(ASFWDiagTopology, nodes) == 1056, "nodes offset mismatch");

static_assert(sizeof(ASFWDiagRoleCoordinator) == 96, "ASFWDiagRoleCoordinator size mismatch");

static_assert(sizeof(ASFWDiagOHCI) == 136, "ASFWDiagOHCI size mismatch");

static_assert(sizeof(ASFWDiagPHY) == 104, "ASFWDiagPHY size mismatch");

static_assert(sizeof(ASFWDiagCSREntry) == 80, "ASFWDiagCSREntry size mismatch");

static_assert(sizeof(ASFWDiagCSRContract) == 2592, "ASFWDiagCSRContract size mismatch");

static_assert(sizeof(ASFWDiagAsyncEvent) == 80, "ASFWDiagAsyncEvent size mismatch");

static_assert(sizeof(ASFWDiagAsyncTrace) == 10280, "ASFWDiagAsyncTrace size mismatch");

static_assert(sizeof(ASFWDiagInboundCSRStats) == 96, "ASFWDiagInboundCSRStats size mismatch");

class DiagnosticsServiceTests : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(DiagnosticsServiceTests, VerifyStructSizeInvariants) {
    // Runtime checks to double check static assertions
    EXPECT_EQ(sizeof(ASFWDiagHeader), 32);
    EXPECT_EQ(sizeof(ASFWDiagBusContract), 96);
    EXPECT_EQ(sizeof(ASFWDiagNode), 164);
    EXPECT_EQ(sizeof(ASFWDiagTopology), 11008);
    EXPECT_EQ(sizeof(ASFWDiagRoleCoordinator), 96);
    EXPECT_EQ(sizeof(ASFWDiagOHCI), 136);
    EXPECT_EQ(sizeof(ASFWDiagPHY), 104);
    EXPECT_EQ(sizeof(ASFWDiagCSREntry), 80);
    EXPECT_EQ(sizeof(ASFWDiagCSRContract), 2592);
    EXPECT_EQ(sizeof(ASFWDiagAsyncEvent), 80);
    EXPECT_EQ(sizeof(ASFWDiagAsyncTrace), 10280);
    EXPECT_EQ(sizeof(ASFWDiagInboundCSRStats), 96);
}

TEST_F(DiagnosticsServiceTests, VerifyEnumValues) {
    EXPECT_EQ(ASFWDiagStatusOK, 0);
    EXPECT_EQ(ASFWDiagStatusUnavailable, 1);
    EXPECT_EQ(ASFWDiagStatusStaleGeneration, 2);
    EXPECT_EQ(ASFWDiagStatusBufferTooSmall, 3);
    EXPECT_EQ(ASFWDiagStatusUnsupported, 4);
    EXPECT_EQ(ASFWDiagStatusBusy, 5);
    EXPECT_EQ(ASFWDiagStatusFailed, 6);
}
