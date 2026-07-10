// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025 ASFW Project

#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "ASFWDriver/Bus/SelfIDStreamParser.hpp"

using namespace ASFW::Driver;

namespace {

// Build a base Self-ID quadlet (single-packet node) with phy_ID and link-active set.
uint32_t MakeBaseSelfID(uint8_t phyId, bool linkActive = true) {
    uint32_t quad = 0x80000000U;                       // Self-ID packet tag
    quad |= (static_cast<uint32_t>(phyId) & 0x3FU) << 24U;
    if (linkActive) {
        quad |= 1U << 22U;                             // L bit
    }
    quad |= 0x3FU << 16U;                              // gap_count = 63 (default)
    quad |= 0x2U << 14U;                               // speed
    return quad;
}

SelfIDCapture::Result MakeResult(std::vector<uint32_t> quads,
                                 std::vector<std::pair<size_t, unsigned int>> sequences) {
    SelfIDCapture::Result result;
    result.valid = true;
    result.quads = std::move(quads);
    result.sequences = std::move(sequences);
    return result;
}

} // namespace

// Three contiguous single-packet nodes parse into three ordered records.
TEST(SelfIDStreamParser, ValidContiguousStream_ParsesAllNodes) {
    auto result = MakeResult(
        {MakeBaseSelfID(0), MakeBaseSelfID(1), MakeBaseSelfID(2)},
        {{0, 1}, {1, 1}, {2, 1}});

    auto records = SelfIDStreamParser::Parse(result);
    ASSERT_TRUE(records.has_value());
    ASSERT_EQ(records->size(), 3u);
    EXPECT_EQ((*records)[0].physicalId, 0);
    EXPECT_EQ((*records)[2].physicalId, 2);
}

// result.valid == false is a low-level stream failure (not a graph error).
TEST(SelfIDStreamParser, InvalidResult_ReturnsInvalidSelfID) {
    SelfIDCapture::Result result;
    result.valid = false;

    auto records = SelfIDStreamParser::Parse(result);
    ASSERT_FALSE(records.has_value());
    EXPECT_EQ(records.error().code, TopologyBuildErrorCode::InvalidSelfID);
}

// Empty quadlet buffer.
TEST(SelfIDStreamParser, EmptyQuads_ReturnsEmptySequenceSet) {
    auto result = MakeResult({}, {});

    auto records = SelfIDStreamParser::Parse(result);
    ASSERT_FALSE(records.has_value());
    EXPECT_EQ(records.error().code, TopologyBuildErrorCode::EmptySequenceSet);
}

// A sequence whose (start + count) runs past the buffer is malformed.
TEST(SelfIDStreamParser, SequenceBoundsExceedBuffer_ReturnsInvalidSelfID) {
    auto result = MakeResult({MakeBaseSelfID(0)}, {{0, 3}});

    auto records = SelfIDStreamParser::Parse(result);
    ASSERT_FALSE(records.has_value());
    EXPECT_EQ(records.error().code, TopologyBuildErrorCode::InvalidSelfID);
}

// Two base packets reporting the same physical_ID.
TEST(SelfIDStreamParser, DuplicatePhysicalId_IsRejected) {
    auto result = MakeResult(
        {MakeBaseSelfID(0), MakeBaseSelfID(0)},
        {{0, 1}, {1, 1}});

    auto records = SelfIDStreamParser::Parse(result);
    ASSERT_FALSE(records.has_value());
    EXPECT_EQ(records.error().code, TopologyBuildErrorCode::DuplicatePhysicalId);
}

// Physical IDs 0 and 2 with no node 1: the stream is not contiguous from 0..root.
TEST(SelfIDStreamParser, NonContiguousPhysicalIds_IsRejected) {
    auto result = MakeResult(
        {MakeBaseSelfID(0), MakeBaseSelfID(2)},
        {{0, 1}, {1, 1}});

    auto records = SelfIDStreamParser::Parse(result);
    ASSERT_FALSE(records.has_value());
    EXPECT_EQ(records.error().code, TopologyBuildErrorCode::NonContiguousPhysicalIds);
}
