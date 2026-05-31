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

// ABI v3: ASFWDiagNode gained parentPort (before ports[]) and links[] (after
// ports[]); ASFWDiagTopology gained gapCount + busBase16 (before rawSelfIds[]).
static_assert(sizeof(ASFWDiagNode) == 276, "ASFWDiagNode size mismatch");
static_assert(offsetof(ASFWDiagNode, parentPort) == 32, "parentPort offset mismatch");
static_assert(offsetof(ASFWDiagNode, ports) == 36, "ports offset mismatch");
static_assert(offsetof(ASFWDiagNode, links) == 144, "links offset mismatch");

static_assert(sizeof(ASFWDiagTopology) == 18760, "ASFWDiagTopology size mismatch");
static_assert(offsetof(ASFWDiagTopology, gapCount) == 64, "gapCount offset mismatch");
static_assert(offsetof(ASFWDiagTopology, busBase16) == 68, "busBase16 offset mismatch");
static_assert(offsetof(ASFWDiagTopology, rawSelfIds) == 72, "rawSelfIds offset mismatch");
static_assert(offsetof(ASFWDiagTopology, nodes) == 1096, "nodes offset mismatch");

static_assert(sizeof(ASFWDiagRoleCoordinator) == 104, "ASFWDiagRoleCoordinator size mismatch");

static_assert(sizeof(ASFWDiagOHCI) == 136, "ASFWDiagOHCI size mismatch");

static_assert(sizeof(ASFWDiagPHY) == 128, "ASFWDiagPHY size mismatch");

static_assert(sizeof(ASFWDiagCSREntry) == 88, "ASFWDiagCSREntry size mismatch");

static_assert(sizeof(ASFWDiagCSRContract) == 2856, "ASFWDiagCSRContract size mismatch");

static_assert(sizeof(ASFWDiagAsyncEvent) == 80, "ASFWDiagAsyncEvent size mismatch");

static_assert(sizeof(ASFWDiagAsyncTrace) == 10280, "ASFWDiagAsyncTrace size mismatch");

static_assert(sizeof(ASFWDiagInboundCSRStats) == 96, "ASFWDiagInboundCSRStats size mismatch");

static_assert(sizeof(ASFWDiagBusManager) == 520, "ASFWDiagBusManager size mismatch");
static_assert(offsetof(ASFWDiagBusManager, header) == 0, "header offset mismatch");
static_assert(offsetof(ASFWDiagBusManager, roleMode) == 32, "roleMode offset mismatch");
static_assert(offsetof(ASFWDiagBusManager, irmFallbackState) == 240, "irmFallbackState offset mismatch");
static_assert(offsetof(ASFWDiagBusManager, cyclePolicyDecision) == 264, "cyclePolicyDecision offset mismatch");
static_assert(offsetof(ASFWDiagBusManager, rootSelectionDecision) == 300, "rootSelectionDecision offset mismatch");
static_assert(offsetof(ASFWDiagBusManager, gapPolicyDecision) == 340, "gapPolicyDecision offset mismatch");
static_assert(offsetof(ASFWDiagBusManager, powerPolicyDecision) == 408, "powerPolicyDecision offset mismatch");

class DiagnosticsServiceTests : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(DiagnosticsServiceTests, VerifyStructSizeInvariants) {
    // Runtime checks to double check static assertions
    EXPECT_EQ(sizeof(ASFWDiagHeader), 32);
    EXPECT_EQ(sizeof(ASFWDiagBusContract), 96);
    EXPECT_EQ(sizeof(ASFWDiagNode), 276);
    EXPECT_EQ(sizeof(ASFWDiagTopology), 18760);
    EXPECT_EQ(sizeof(ASFWDiagRoleCoordinator), 104);
    EXPECT_EQ(sizeof(ASFWDiagOHCI), 136);
    EXPECT_EQ(sizeof(ASFWDiagPHY), 128);
    EXPECT_EQ(sizeof(ASFWDiagCSREntry), 88);
    EXPECT_EQ(sizeof(ASFWDiagCSRContract), 2856);
    EXPECT_EQ(sizeof(ASFWDiagAsyncEvent), 80);
    EXPECT_EQ(sizeof(ASFWDiagAsyncTrace), 10280);
    EXPECT_EQ(sizeof(ASFWDiagInboundCSRStats), 96);
    EXPECT_EQ(sizeof(ASFWDiagBusManager), 520);
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
