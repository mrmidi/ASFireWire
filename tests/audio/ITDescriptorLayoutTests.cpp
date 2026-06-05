// Portions of these tests (IsochHeader and ITDescriptorBuilder layout validations)
// are derived/ported from the Linux FireWire subsystem KUnit tests (ohci-serdes-test.c),
// Copyright (c) 2024 Takashi Sakamoto.
// Preserving authorship for the Linux KUnit-derived test assertions.

#include <gtest/gtest.h>
#include "ASFWDriver/Hardware/OHCIDescriptors.hpp"

using namespace ASFW::Async::HW;

TEST(ITDescriptorLayoutTests, IsochHeaderBuild) {
    // tag = 2, chan = 0x1F, tcode = 0xA, sy = 0x5
    uint32_t val = IsochHeader::Build(2, 0x1F, 0xA, 0x5);

    // Expected shifts/values:
    // (tag & 0x3) << 14 -> (2 & 0x3) << 14 = 0x8000
    // (chan & 0x3F) << 8 -> (0x1F & 0x3F) << 8 = 0x1F00
    // (tcode & 0xF) << 4 -> (0xA & 0xF) << 4 = 0x00A0
    // (sy & 0xF) -> 0x5
    // Result: 0x8000 | 0x1F00 | 0x00A0 | 0x5 = 0x9FA5
    EXPECT_EQ(val, 0x9FA5u);

    // Test masking of out of range fields
    uint32_t valMasked = IsochHeader::Build(0xFF, 0xFF, 0xFF, 0xFF);
    // (0xFF & 0x3) << 14 -> 3 << 14 = 0xC000
    // (0xFF & 0x3F) << 8 -> 0x3F << 8 = 0x3F00
    // (0xFF & 0xF) << 4 -> 0xF << 4 = 0x00F0
    // (0xFF & 0xF) -> 0xF
    // Result: 0xC000 | 0x3F00 | 0x00F0 | 0xF = 0xFFF0 | 0xF = 0xFFFF
    EXPECT_EQ(valMasked, 0xFFFFu);
}

TEST(ITDescriptorLayoutTests, BuildOutputMoreImmediate) {
    OHCIDescriptorImmediate desc{};
    ITDescriptorBuilder::OutputMoreImmediateParams params{};
    params.isochHeaderLE = 0x11223344u;
    params.cipQ0LE = 0x55667788u;
    params.interruptBits = OHCIDescriptor::kIntAlways;

    ITDescriptorBuilder::BuildOutputMoreImmediate(desc, params);

    // Control word checks:
    // reqCount = 4
    // cmd = OutputMore (0x0)
    // key = Immediate (0x2)
    // interruptBits = kIntAlways (3)
    // branchBits = kBranchNever (0)
    // BuildControl uses:
    // high = (0x0 << 12) | (0x2 << 8) | (0x3 << 4) | (0x0 << 2) = 0x0230
    // control = (0x0230 << 16) | 4 = 0x02300004
    EXPECT_EQ(desc.common.control, 0x02300004u);

    // dataAddress should be cleared
    EXPECT_EQ(desc.common.dataAddress, 0u);

    // branchWord maps to Imm0 (IsochHeader)
    EXPECT_EQ(desc.common.branchWord, 0x11223344u);

    // statusWord maps to Imm1 (CIP Q0)
    EXPECT_EQ(desc.common.statusWord, 0x55667788u);

    // Unused immediateData quadlets (0 to 3) should be zero
    EXPECT_EQ(desc.immediateData[0], 0u);
    EXPECT_EQ(desc.immediateData[1], 0u);
    EXPECT_EQ(desc.immediateData[2], 0u);
    EXPECT_EQ(desc.immediateData[3], 0u);
}

TEST(ITDescriptorLayoutTests, BuildOutputLast) {
    OHCIDescriptor desc{};
    ITDescriptorBuilder::OutputLastParams params{};
    params.dataIOVA = 0xAABBCC00u;
    params.payloadSize = 512;
    params.branchIOVA = 0x12345000u;
    params.zValue = 4;
    params.interruptBits = OHCIDescriptor::kIntAlways;

    ITDescriptorBuilder::BuildOutputLast(desc, params);

    // Control word checks:
    // reqCount = 512 (0x0200)
    // cmd = OutputLast (0x1)
    // key = Standard (0x0)
    // interruptBits = kIntAlways (3)
    // branchBits = kBranchAlways (3)
    // s = 1 (status update) -> bit (11 + 16) = bit 27 = 0x08000000
    // BuildControl high:
    // (0x1 << 12) | (0x0 << 8) | (0x3 << 4) | (0x3 << 2) = 0x103C
    // control initial = (0x103C << 16) | 0x0200 = 0x103C0200
    // s=1 set -> control = 0x103C0200 | 0x08000000 = 0x183C0200
    EXPECT_EQ(desc.control, 0x183C0200u);

    // dataAddress points to payload size
    EXPECT_EQ(desc.dataAddress, 0xAABBCC00u);

    // branchWord encodes branchIOVA and Z value:
    // MakeBranchWordAT(0x12345000u, 4) -> 0x12345004
    EXPECT_EQ(desc.branchWord, 0x12345004u);

    // statusWord initialized to payloadSize (reqCount_host)
    EXPECT_EQ(desc.statusWord, 512u);
}
