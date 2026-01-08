/**
 * ClusterInfoParsingTests.cpp
 *
 * Tests for ClusterInfo (0x810A) parsing from Music Subunit descriptors.
 * Uses real Apogee Duet descriptor data from duet.bin.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <string>

// Include the AVCInfoBlock parser
#include "Protocols/AVC/Descriptors/AVCInfoBlock.hpp"

using namespace ASFW::Protocols::AVC::Descriptors;

namespace {

// Real Apogee Duet Music Subunit Status Descriptor (464 bytes)
// Captured from device via descriptor read
static const uint8_t kDuetDescriptor[] = {
  0x01, 0xce, 0x00, 0x0a, 0x81, 0x00, 0x00, 0x06, 0x01, 0x01, 0xff, 0xff,
  0xff, 0xff, 0x01, 0xc0, 0x81, 0x08, 0x00, 0x04, 0x03, 0x03, 0x00, 0x05,
  0x00, 0x2e, 0x81, 0x09, 0x00, 0x08, 0x00, 0x90, 0x01, 0x00, 0x00, 0x01,
  0x00, 0x02, 0x00, 0x20, 0x81, 0x0a, 0x00, 0x0b, 0x06, 0x03, 0x02, 0x00,
  0x00, 0x00, 0xff, 0x00, 0x01, 0x01, 0xff, 0x00, 0x0f, 0x00, 0x0a, 0x00,
  0x0b, 0x41, 0x6e, 0x61, 0x6c, 0x6f, 0x67, 0x20, 0x4f, 0x75, 0x74, 0x00,
  0x00, 0x2d, 0x81, 0x09, 0x00, 0x08, 0x01, 0x90, 0x01, 0x05, 0x00, 0x01,
  0x00, 0x02, 0x00, 0x1f, 0x81, 0x0a, 0x00, 0x0b, 0x06, 0x03, 0x02, 0x00,
  0x02, 0x00, 0xff, 0x00, 0x03, 0x01, 0xff, 0x00, 0x0e, 0x00, 0x0a, 0x00,
  0x0a, 0x41, 0x6e, 0x61, 0x6c, 0x6f, 0x67, 0x20, 0x49, 0x6e, 0x00, 0x00,
  0x24, 0x81, 0x09, 0x00, 0x08, 0x02, 0x90, 0x01, 0x03, 0x00, 0x01, 0x00,
  0x01, 0x00, 0x16, 0x81, 0x0a, 0x00, 0x07, 0x40, 0x09, 0x01, 0x00, 0x04,
  0x00, 0xff, 0x00, 0x09, 0x00, 0x0a, 0x00, 0x05, 0x53, 0x79, 0x6e, 0x63,
  0x00, 0x00, 0x2d, 0x81, 0x09, 0x00, 0x08, 0x00, 0x90, 0x01, 0x00, 0x00,
  0x01, 0x00, 0x02, 0x00, 0x1f, 0x81, 0x0a, 0x00, 0x0b, 0x06, 0x03, 0x02,
  0x00, 0x02, 0x00, 0xff, 0x00, 0x03, 0x01, 0xff, 0x00, 0x0e, 0x00, 0x0a,
  0x00, 0x0a, 0x41, 0x6e, 0x61, 0x6c, 0x6f, 0x67, 0x20, 0x49, 0x6e, 0x00,
  0x00, 0x2e, 0x81, 0x09, 0x00, 0x08, 0x01, 0x90, 0x01, 0x05, 0x00, 0x01,
  0x00, 0x02, 0x00, 0x20, 0x81, 0x0a, 0x00, 0x0b, 0x06, 0x03, 0x02, 0x00,
  0x00, 0x00, 0xff, 0x00, 0x01, 0x01, 0xff, 0x00, 0x0f, 0x00, 0x0a, 0x00,
  0x0b, 0x41, 0x6e, 0x61, 0x6c, 0x6f, 0x67, 0x20, 0x4f, 0x75, 0x74, 0x00,
  0x00, 0x24, 0x81, 0x09, 0x00, 0x08, 0x02, 0x90, 0x01, 0x03, 0x00, 0x01,
  0x00, 0x01, 0x00, 0x16, 0x81, 0x0a, 0x00, 0x07, 0x40, 0x09, 0x01, 0x00,
  0x04, 0x00, 0xff, 0x00, 0x09, 0x00, 0x0a, 0x00, 0x05, 0x53, 0x79, 0x6e,
  0x63, 0x00, 0x00, 0x25, 0x81, 0x0b, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00,
  0xf0, 0x00, 0xff, 0x00, 0xff, 0xf1, 0x01, 0xff, 0x00, 0xff, 0x00, 0x11,
  0x00, 0x0a, 0x00, 0x0d, 0x41, 0x6e, 0x61, 0x6c, 0x6f, 0x67, 0x20, 0x4f,
  0x75, 0x74, 0x20, 0x31, 0x00, 0x00, 0x25, 0x81, 0x0b, 0x00, 0x0e, 0x00,
  0x00, 0x01, 0x00, 0xf0, 0x00, 0xff, 0x01, 0xff, 0xf1, 0x01, 0xff, 0x01,
  0xff, 0x00, 0x11, 0x00, 0x0a, 0x00, 0x0d, 0x41, 0x6e, 0x61, 0x6c, 0x6f,
  0x67, 0x20, 0x4f, 0x75, 0x74, 0x20, 0x32, 0x00, 0x00, 0x24, 0x81, 0x0b,
  0x00, 0x0e, 0x00, 0x00, 0x02, 0x00, 0xf0, 0x01, 0xff, 0x00, 0xff, 0xf1,
  0x00, 0xff, 0x00, 0xff, 0x00, 0x10, 0x00, 0x0a, 0x00, 0x0c, 0x41, 0x6e,
  0x61, 0x6c, 0x6f, 0x67, 0x20, 0x49, 0x6e, 0x20, 0x31, 0x00, 0x00, 0x24,
  0x81, 0x0b, 0x00, 0x0e, 0x00, 0x00, 0x03, 0x00, 0xf0, 0x01, 0xff, 0x01,
  0xff, 0xf1, 0x00, 0xff, 0x01, 0xff, 0x00, 0x10, 0x00, 0x0a, 0x00, 0x0c,
  0x41, 0x6e, 0x61, 0x6c, 0x6f, 0x67, 0x20, 0x49, 0x6e, 0x20, 0x32, 0x00,
  0x00, 0x12, 0x81, 0x0b, 0x00, 0x0e, 0x80, 0x00, 0x04, 0x00, 0xf0, 0x02,
  0xff, 0x00, 0xff, 0xf1, 0x02, 0xff, 0x01, 0xce
};

constexpr size_t kDuetDescriptorLen = sizeof(kDuetDescriptor);

// Block type constants
constexpr uint16_t kBlockTypeRoutingStatus = 0x8108;
constexpr uint16_t kBlockTypeSubunitPlugInfo = 0x8109;
constexpr uint16_t kBlockTypeClusterInfo = 0x810A;
constexpr uint16_t kBlockTypeMusicPlugInfo = 0x810B;

class ClusterInfoParsingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Skip 2-byte descriptor length prefix
        const uint8_t* data = kDuetDescriptor + 2;
        size_t length = kDuetDescriptorLen - 2;
        
        // The descriptor contains MULTIPLE SEQUENTIAL info blocks at the top level
        // (not a single root with nested blocks)
        // Parse all blocks until we exhaust the data
        size_t offset = 0;
        while (offset < length) {
            size_t consumed = 0;
            auto result = AVCInfoBlock::Parse(data + offset, length - offset, consumed);
            if (result.has_value()) {
                allBlocks_.push_back(std::move(result.value()));
                offset += consumed;
                parseSuccess_ = true;
            } else {
                break;  // Stop on first parse error
            }
        }
    }
    
    // Find blocks by type across all parsed blocks
    std::vector<AVCInfoBlock> FindAllByType(uint16_t type) const {
        std::vector<AVCInfoBlock> matches;
        for (const auto& block : allBlocks_) {
            if (block.GetType() == type) {
                matches.push_back(block);
            }
            // Also search nested
            auto nested = block.FindAllNestedRecursive(type);
            matches.insert(matches.end(), nested.begin(), nested.end());
        }
        return matches;
    }
    
    std::vector<AVCInfoBlock> allBlocks_;
    bool parseSuccess_ = false;
};

TEST_F(ClusterInfoParsingTest, ParsesRoutingStatusBlock) {
    ASSERT_TRUE(parseSuccess_) << "Parsing should succeed";
    ASSERT_FALSE(allBlocks_.empty()) << "Should have parsed blocks";
    
    // Should have at least 2 blocks: GeneralMusicSubunitStatusArea + RoutingStatus
    EXPECT_GE(allBlocks_.size(), 2u) << "Should have at least 2 top-level blocks";
    
    // Find RoutingStatus (0x8108) block
    auto routingBlocks = FindAllByType(kBlockTypeRoutingStatus);
    ASSERT_EQ(routingBlocks.size(), 1u) << "Should find exactly 1 RoutingStatus block";
    
    const auto& routing = routingBlocks[0];
    const auto& primaryData = routing.GetPrimaryData();
    
    // RoutingStatus primary fields: [0]=numDestPlugs, [1]=numSourcePlugs
    ASSERT_GE(primaryData.size(), 2u);
    EXPECT_EQ(primaryData[0], 3) << "Should have 3 destination plugs";
    EXPECT_EQ(primaryData[1], 3) << "Should have 3 source plugs";
}

TEST_F(ClusterInfoParsingTest, FindsSubunitPlugInfoBlocks) {
    ASSERT_TRUE(parseSuccess_);
    
    // Find all SubunitPlugInfo (0x8109) blocks
    auto plugBlocks = FindAllByType(kBlockTypeSubunitPlugInfo);
    
    // Duet has 3 dest + 3 src = 6 SubunitPlugInfo blocks
    EXPECT_EQ(plugBlocks.size(), 6u) << "Should find 6 SubunitPlugInfo blocks";
}

TEST_F(ClusterInfoParsingTest, FindsClusterInfoBlocks) {
    ASSERT_TRUE(parseSuccess_);
    
    // Find all ClusterInfo (0x810A) blocks
    auto clusterBlocks = FindAllByType(kBlockTypeClusterInfo);
    
    // Each SubunitPlugInfo contains a ClusterInfo (6 total)
    EXPECT_EQ(clusterBlocks.size(), 6u) << "Should find 6 ClusterInfo blocks";
}

TEST_F(ClusterInfoParsingTest, ParsesClusterInfoSignals) {
    ASSERT_TRUE(parseSuccess_);
    
    auto clusterBlocks = FindAllByType(kBlockTypeClusterInfo);
    ASSERT_GE(clusterBlocks.size(), 1u);
    
    // First ClusterInfo should be for "Analog Out" plug with 2 channels
    const auto& cluster = clusterBlocks[0];
    const auto& primaryData = cluster.GetPrimaryData();
    
    // ClusterInfo primary fields:
    // [0]=formatCode (0x06=MBLA), [1]=portType, [2]=numSignals
    // Then 4 bytes per signal: musicPlugID(2), channel(1), location(1)
    ASSERT_GE(primaryData.size(), 3u);
    
    uint8_t formatCode = primaryData[0];
    uint8_t numSignals = primaryData[2];
    
    EXPECT_EQ(formatCode, 0x06) << "Format should be MBLA (0x06)";
    EXPECT_EQ(numSignals, 2) << "Should have 2 signals (channels)";
    
    // Verify signal data present: 3 + (4 * numSignals) bytes needed
    size_t expectedSize = 3 + (4 * numSignals);
    EXPECT_GE(primaryData.size(), expectedSize) 
        << "Primary data should have signal entries";
    
    // Parse signal 0
    if (primaryData.size() >= 7) {
        uint16_t sig0_musicPlugID = (static_cast<uint16_t>(primaryData[3]) << 8) | primaryData[4];
        
        EXPECT_EQ(sig0_musicPlugID, 0x0000) << "Signal 0 musicPlugID should be 0";
    }
    
    // Parse signal 1
    if (primaryData.size() >= 11) {
        uint16_t sig1_musicPlugID = (static_cast<uint16_t>(primaryData[7]) << 8) | primaryData[8];
        
        EXPECT_EQ(sig1_musicPlugID, 0x0001) << "Signal 1 musicPlugID should be 1";
    }
}

TEST_F(ClusterInfoParsingTest, FindsMusicPlugInfoBlocks) {
    ASSERT_TRUE(parseSuccess_);
    
    // Find all MusicPlugInfo (0x810B) blocks
    auto musicPlugBlocks = FindAllByType(kBlockTypeMusicPlugInfo);
    
    // Duet has 5 MusicPlugInfo blocks (4 analog + 1 sync)
    EXPECT_GE(musicPlugBlocks.size(), 4u) << "Should find at least 4 MusicPlugInfo blocks";
}

TEST_F(ClusterInfoParsingTest, ParsesMusicPlugInfoNames) {
    ASSERT_TRUE(parseSuccess_);
    
    auto musicPlugBlocks = FindAllByType(kBlockTypeMusicPlugInfo);
    ASSERT_GE(musicPlugBlocks.size(), 1u);
    
    // First MusicPlugInfo should have name "Analog Out 1"
    const auto& musicPlug = musicPlugBlocks[0];
    const auto& primaryData = musicPlug.GetPrimaryData();
    
    // MusicPlugInfo primary fields:
    // [0]=portType, [1-2]=musicPlugID (BE), [3]=routingSupport, ...
    ASSERT_GE(primaryData.size(), 3u);
    
    uint8_t portType = primaryData[0];
    uint16_t musicPlugID = (static_cast<uint16_t>(primaryData[1]) << 8) | primaryData[2];
    
    EXPECT_EQ(portType, 0x00) << "Port type should be Audio (0x00)";
    EXPECT_EQ(musicPlugID, 0x0000) << "First MusicPlugInfo ID should be 0";
    
    // Look for name in nested RawText (0x000A) block
    auto nameBlock = musicPlug.FindNestedRecursive(0x000A);
    ASSERT_TRUE(nameBlock.has_value()) << "Should have nested RawText block";
    
    const auto& nameData = nameBlock->GetPrimaryData();
    ASSERT_FALSE(nameData.empty());
    
    std::string name(reinterpret_cast<const char*>(nameData.data()), nameData.size());
    // Remove non-printable characters
    name.erase(std::remove_if(name.begin(), name.end(), 
        [](unsigned char c) { return !std::isprint(c); }), name.end());
    
    EXPECT_EQ(name, "Analog Out 1") << "First channel name should be 'Analog Out 1'";
}

TEST_F(ClusterInfoParsingTest, ClusterInfoNestedInSubunitPlugInfo) {
    ASSERT_TRUE(parseSuccess_);
    
    // Find first SubunitPlugInfo
    auto plugBlocks = FindAllByType(kBlockTypeSubunitPlugInfo);
    ASSERT_GE(plugBlocks.size(), 1u);
    
    const auto& firstPlug = plugBlocks[0];
    
    // ClusterInfo should be nested directly inside SubunitPlugInfo
    auto clusterBlocks = firstPlug.FindAllNestedRecursive(kBlockTypeClusterInfo);
    EXPECT_EQ(clusterBlocks.size(), 1u) << "Each SubunitPlugInfo should have 1 ClusterInfo";
}

} // anonymous namespace
