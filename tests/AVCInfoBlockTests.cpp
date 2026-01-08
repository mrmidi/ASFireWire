#include <gtest/gtest.h>
#include "ASFWDriver/Protocols/AVC/Descriptors/AVCInfoBlock.hpp"
#include "ASFWDriver/Protocols/AVC/AVCDefs.hpp"
#include <vector>

using namespace ASFW::Protocols::AVC;
using namespace ASFW::Protocols::AVC::Descriptors;

class AVCInfoBlockTests : public ::testing::Test {
protected:
    // Helper to create big-endian uint16_t bytes
    static void WriteBE16(std::vector<uint8_t>& data, uint16_t value) {
        data.push_back((value >> 8) & 0xFF);
        data.push_back(value & 0xFF);
    }

    // Helper to create a simple info block with no nested blocks
    static std::vector<uint8_t> CreateSimpleBlock(
        uint16_t type,
        const std::vector<uint8_t>& primaryData
    ) {
        std::vector<uint8_t> data;

        // compound_length = 4 (Type+PFL) + primary_data.size()
        // Note: compound_length excludes the length field itself (2 bytes)
        uint16_t compoundLength = 4 + primaryData.size();
        WriteBE16(data, compoundLength);

        // type (Offset 2)
        WriteBE16(data, type);

        // primary_fields_length (Offset 4)
        WriteBE16(data, primaryData.size());

        // primary data
        data.insert(data.end(), primaryData.begin(), primaryData.end());

        return data;
    }

    // Helper to create a block with nested blocks
    static std::vector<uint8_t> CreateBlockWithNested(
        uint16_t type,
        const std::vector<uint8_t>& primaryData,
        const std::vector<std::vector<uint8_t>>& nestedBlocks
    ) {
        std::vector<uint8_t> data;

        // Calculate total size
        size_t nestedSize = 0;
        for (const auto& block : nestedBlocks) {
            nestedSize += block.size();
        }

        // compound_length = 4 (Type+PFL) + primary + nested
        uint16_t compoundLength = 4 + primaryData.size() + nestedSize;
        WriteBE16(data, compoundLength);

        // type (Offset 2)
        WriteBE16(data, type);

        // primary_fields_length (Offset 4)
        WriteBE16(data, primaryData.size());

        // primary data
        data.insert(data.end(), primaryData.begin(), primaryData.end());

        // nested blocks
        for (const auto& block : nestedBlocks) {
            data.insert(data.end(), block.begin(), block.end());
        }

        return data;
    }
};

//==============================================================================
// Basic Parsing Tests
//==============================================================================

TEST_F(AVCInfoBlockTests, ParseTooShort) {
    std::vector<uint8_t> data = {0x00, 0x01, 0x02}; // Only 3 bytes, need 6

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(data.data(), data.size(), consumed);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AVCResult::kInvalidResponse);
    EXPECT_EQ(consumed, 0);
}

TEST_F(AVCInfoBlockTests, ParseMinimalBlock) {
    // Create empty block (type 0x1234, no primary data)
    auto data = CreateSimpleBlock(0x1234, {});

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetType(), 0x1234);
    EXPECT_EQ(result->GetCompoundLength(), 4); // 4 bytes (Type + PFL) excluding length field
    EXPECT_EQ(result->GetPrimaryFieldsLength(), 0);
    EXPECT_TRUE(result->GetPrimaryData().empty());
    EXPECT_FALSE(result->HasNestedBlocks());
    EXPECT_EQ(consumed, 6); // 2 (Length) + 4 (Body)
}

TEST_F(AVCInfoBlockTests, ParseBlockWithPrimaryData) {
    // Create block with primary data
    std::vector<uint8_t> primaryData = {0xAA, 0xBB, 0xCC, 0xDD};
    auto data = CreateSimpleBlock(0x5678, primaryData);

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(data.data(), data.size(), consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetType(), 0x5678);
    EXPECT_EQ(result->GetCompoundLength(), 8); // 4 + 4
    EXPECT_EQ(result->GetPrimaryFieldsLength(), 4);
    EXPECT_EQ(result->GetPrimaryData(), primaryData);
    EXPECT_FALSE(result->HasNestedBlocks());
    EXPECT_EQ(consumed, 10);
}

TEST_F(AVCInfoBlockTests, InvalidCompoundLength) {
    std::vector<uint8_t> data = {
        0x00, 0x03,  // compound_length = 3 (invalid, must be >= 4)
        0x00, 0x00,  // primary_fields_length = 0
        0x12, 0x34   // type
    };

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(data.data(), data.size(), consumed);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AVCResult::kInvalidResponse);
}

TEST_F(AVCInfoBlockTests, InvalidPrimaryFieldsLength) {
    std::vector<uint8_t> data = {
        0x00, 0x08,  // compound_length = 8
        0x12, 0x34,  // type
        0x00, 0x10,  // primary_fields_length = 16 (exceeds compound_length - 4 = 4)
        0x00, 0x00, 0x00, 0x00 // 4 bytes of data
    };

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(data.data(), data.size(), consumed);

    // Robust parser should truncate PFL and succeed
    EXPECT_TRUE(result.has_value());
}

//==============================================================================
// Nested Block Parsing Tests
//==============================================================================

TEST_F(AVCInfoBlockTests, ParseSingleNestedBlock) {
    // Create nested block
    auto nestedBlock1 = CreateSimpleBlock(0x1111, {0xAA});

    // Create parent block with nested
    auto parentBlock = CreateBlockWithNested(0x9999, {0xFF}, {nestedBlock1});

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(parentBlock.data(), parentBlock.size(), consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetType(), 0x9999);
    EXPECT_EQ(result->GetPrimaryData().size(), 1);
    EXPECT_EQ(result->GetPrimaryData()[0], 0xFF);
    EXPECT_TRUE(result->HasNestedBlocks());
    EXPECT_EQ(result->GetNestedBlocks().size(), 1);

    const auto& nested = result->GetNestedBlocks()[0];
    EXPECT_EQ(nested.GetType(), 0x1111);
    EXPECT_EQ(nested.GetPrimaryData().size(), 1);
    EXPECT_EQ(nested.GetPrimaryData()[0], 0xAA);
}

TEST_F(AVCInfoBlockTests, ParseMultipleNestedBlocks) {
    // Create 3 nested blocks
    auto nested1 = CreateSimpleBlock(0x0001, {0x11});
    auto nested2 = CreateSimpleBlock(0x0002, {0x22, 0x23});
    auto nested3 = CreateSimpleBlock(0x0003, {0x33, 0x34, 0x35});

    // Create parent with all 3 nested
    auto parent = CreateBlockWithNested(0xAAAA, {}, {nested1, nested2, nested3});

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(parent.data(), parent.size(), consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetType(), 0xAAAA);
    EXPECT_TRUE(result->GetPrimaryData().empty());
    ASSERT_EQ(result->GetNestedBlocks().size(), 3);

    EXPECT_EQ(result->GetNestedBlocks()[0].GetType(), 0x0001);
    EXPECT_EQ(result->GetNestedBlocks()[1].GetType(), 0x0002);
    EXPECT_EQ(result->GetNestedBlocks()[2].GetType(), 0x0003);
}

TEST_F(AVCInfoBlockTests, ParseDeeplyNestedBlocks) {
    // Create deeply nested structure: Level3 -> Level2 -> Level1 -> Root
    auto level3 = CreateSimpleBlock(0x0003, {0x33});
    auto level2 = CreateBlockWithNested(0x0002, {0x22}, {level3});
    auto level1 = CreateBlockWithNested(0x0001, {0x11}, {level2});
    auto root = CreateBlockWithNested(0x0000, {}, {level1});

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(root.data(), root.size(), consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetType(), 0x0000);
    ASSERT_EQ(result->GetNestedBlocks().size(), 1);

    const auto& l1 = result->GetNestedBlocks()[0];
    EXPECT_EQ(l1.GetType(), 0x0001);
    ASSERT_EQ(l1.GetNestedBlocks().size(), 1);

    const auto& l2 = l1.GetNestedBlocks()[0];
    EXPECT_EQ(l2.GetType(), 0x0002);
    ASSERT_EQ(l2.GetNestedBlocks().size(), 1);

    const auto& l3 = l2.GetNestedBlocks()[0];
    EXPECT_EQ(l3.GetType(), 0x0003);
    EXPECT_FALSE(l3.HasNestedBlocks());
}

//==============================================================================
// Navigation Helper Tests
//==============================================================================

TEST_F(AVCInfoBlockTests, FindNested) {
    auto nested1 = CreateSimpleBlock(0x1111, {0x11});
    auto nested2 = CreateSimpleBlock(0x2222, {0x22});
    auto nested3 = CreateSimpleBlock(0x3333, {0x33});

    auto parent = CreateBlockWithNested(0x9999, {}, {nested1, nested2, nested3});

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(parent.data(), parent.size(), consumed);
    ASSERT_TRUE(result.has_value());

    // Find existing types
    auto found = result->FindNested(0x2222);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->GetType(), 0x2222);
    EXPECT_EQ(found->GetPrimaryData()[0], 0x22);

    // Find non-existent type
    auto notFound = result->FindNested(0xFFFF);
    EXPECT_FALSE(notFound.has_value());
}

TEST_F(AVCInfoBlockTests, FindAllNested) {
    // Create multiple blocks with same type
    auto block1 = CreateSimpleBlock(0x1111, {0x01});
    auto block2 = CreateSimpleBlock(0x2222, {0x02});
    auto block3 = CreateSimpleBlock(0x1111, {0x03});  // Duplicate type
    auto block4 = CreateSimpleBlock(0x1111, {0x04});  // Another duplicate

    auto parent = CreateBlockWithNested(0x9999, {}, {block1, block2, block3, block4});

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(parent.data(), parent.size(), consumed);
    ASSERT_TRUE(result.has_value());

    // Find all blocks of type 0x1111
    auto matches = result->FindAllNested(0x1111);
    ASSERT_EQ(matches.size(), 3);
    EXPECT_EQ(matches[0].GetPrimaryData()[0], 0x01);
    EXPECT_EQ(matches[1].GetPrimaryData()[0], 0x03);
    EXPECT_EQ(matches[2].GetPrimaryData()[0], 0x04);

    // Find all blocks of type 0x2222
    auto single = result->FindAllNested(0x2222);
    ASSERT_EQ(single.size(), 1);
    EXPECT_EQ(single[0].GetPrimaryData()[0], 0x02);

    // Find non-existent type
    auto empty = result->FindAllNested(0xFFFF);
    EXPECT_TRUE(empty.empty());
}

TEST_F(AVCInfoBlockTests, FindNestedRecursive) {
    // Create structure where target is deeply nested
    auto target = CreateSimpleBlock(0xAAAA, {0xAA});
    auto level2 = CreateBlockWithNested(0x0002, {}, {target});
    auto level1 = CreateBlockWithNested(0x0001, {}, {level2});

    // Also add a non-matching nested block at level 1
    auto other = CreateSimpleBlock(0xBBBB, {0xBB});

    auto root = CreateBlockWithNested(0x0000, {}, {level1, other});

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(root.data(), root.size(), consumed);
    ASSERT_TRUE(result.has_value());

    // Recursive search should find deeply nested block
    auto found = result->FindNestedRecursive(0xAAAA);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->GetType(), 0xAAAA);
    EXPECT_EQ(found->GetPrimaryData()[0], 0xAA);

    // Non-recursive search should NOT find it
    auto notFound = result->FindNested(0xAAAA);
    EXPECT_FALSE(notFound.has_value());

    // But should find immediate children
    auto immediate = result->FindNested(0xBBBB);
    ASSERT_TRUE(immediate.has_value());
    EXPECT_EQ(immediate->GetType(), 0xBBBB);
}

//==============================================================================
// Real-World Pattern Tests (Music Subunit Status Descriptor)
//==============================================================================

TEST_F(AVCInfoBlockTests, MusicSubunitPlugInfoPattern) {
    // Simulate Music Subunit Plug Info block (type 0x8109)
    // Primary: PlugID, SignalFmt(2), Type, Clusters(2), Channels(2)
    std::vector<uint8_t> plugPrimary = {
        0x00,        // Plug ID = 0
        0x90, 0x40,  // Signal Format (IEC60958-3, 48kHz)
        0x00,        // Type (destination/input)
        0x00, 0x01,  // Clusters = 1
        0x00, 0x02   // Channels = 2
    };

    // Nested: Name block (type 0x000D) with text
    std::vector<uint8_t> nameText = {
        'A', 'n', 'a', 'l', 'o', 'g', ' ', 'I', 'n'
    };
    auto nameBlock = CreateSimpleBlock(0x000D, nameText);

    auto plugBlock = CreateBlockWithNested(0x8109, plugPrimary, {nameBlock});

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(plugBlock.data(), plugBlock.size(), consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetType(), 0x8109);
    EXPECT_EQ(result->GetPrimaryData().size(), plugPrimary.size());

    // Extract plug info from primary data
    const auto& primary = result->GetPrimaryData();
    EXPECT_EQ(primary[0], 0x00); // Plug ID
    EXPECT_EQ(primary[1], 0x90); // Format
    EXPECT_EQ(primary[3], 0x00); // Type (input)

    // Find name block
    auto name = result->FindNested(0x000D);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name->GetPrimaryData(), nameText);
}

//==============================================================================
// Edge Cases
//==============================================================================

TEST_F(AVCInfoBlockTests, TruncatedNestedBlock) {
    std::vector<uint8_t> data;

    // Parent header
    WriteBE16(data, 20);  // compound_length (claims 20 bytes)
    WriteBE16(data, 0x9999);  // type
    WriteBE16(data, 2);   // primary_fields_length
    data.push_back(0xAA);
    data.push_back(0xBB);

    // Start of nested block, but truncated
    WriteBE16(data, 10);  // compound_length (10 bytes)
    WriteBE16(data, 0x1111); // type
    WriteBE16(data, 2);   // primary_fields_length
    // Missing data!

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(data.data(), data.size(), consumed);

    // Should parse the parent, nested block parsing stops gracefully
    EXPECT_TRUE(result.has_value());
    // Check that the nested block list is empty or contains valid parts
}

TEST_F(AVCInfoBlockTests, ExtraDataAfterBlock) {
    auto block = CreateSimpleBlock(0x1234, {0xAA, 0xBB});

    // Add extra data after the block
    block.push_back(0xFF);
    block.push_back(0xFF);

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(block.data(), block.size(), consumed);

    // Should parse successfully and only consume the block size
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(consumed, 8); // 2 (Len) + 2 (Type) + 2 (PFL) + 2 (Data)
    EXPECT_LT(consumed, block.size());
}

//==============================================================================
// RoutingStatus (0x8108) Tests - Plug Direction from Position
// Based on Apple's VirtualMusicSubunit.cpp: first numDestPlugs are Input,
// next numSourcePlugs are Output
//==============================================================================

TEST_F(AVCInfoBlockTests, RoutingStatus_PrimaryFieldsParsing) {
    // RoutingStatus primary fields: [numDestPlugs, numSourcePlugs, musicPlugCountMSB, musicPlugCountLSB]
    std::vector<uint8_t> routingPrimary = {
        0x03,        // numDestPlugs = 3 (Input/Destination plugs)
        0x02,        // numSourcePlugs = 2 (Output/Source plugs)
        0x00, 0x05   // musicPlugCount = 5
    };

    auto routingBlock = CreateSimpleBlock(0x8108, routingPrimary);

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(routingBlock.data(), routingBlock.size(), consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetType(), 0x8108);
    
    const auto& primary = result->GetPrimaryData();
    ASSERT_GE(primary.size(), 4);
    EXPECT_EQ(primary[0], 3); // numDestPlugs
    EXPECT_EQ(primary[1], 2); // numSourcePlugs
    EXPECT_EQ((primary[2] << 8) | primary[3], 5); // musicPlugCount
}

TEST_F(AVCInfoBlockTests, RoutingStatus_PlugDirectionFromPosition) {
    // Create a RoutingStatus block with 2 dest plugs and 1 source plug
    // Per Apple's VirtualMusicSubunit:
    // - First 2 SubunitPlugInfo blocks should be Input (Destination)
    // - Next 1 SubunitPlugInfo block should be Output (Source)
    
    std::vector<uint8_t> routingPrimary = {
        0x02,        // numDestPlugs = 2
        0x01,        // numSourcePlugs = 1
        0x00, 0x00   // musicPlugCount = 0
    };

    // SubunitPlugInfo primary: [subunit_plug_id, fdf_fmt1, fdf_fmt2, usage, ...]
    // Plug 0 - should be Input (first dest plug)
    std::vector<uint8_t> plug0Primary = {0x00, 0x90, 0x40, 0x04, 0x00, 0x01, 0x00, 0x02};
    auto plug0 = CreateSimpleBlock(0x8109, plug0Primary);
    
    // Plug 1 - should be Input (second dest plug)
    std::vector<uint8_t> plug1Primary = {0x01, 0x90, 0x40, 0x04, 0x00, 0x01, 0x00, 0x02};
    auto plug1 = CreateSimpleBlock(0x8109, plug1Primary);
    
    // Plug 2 - should be Output (first source plug)
    std::vector<uint8_t> plug2Primary = {0x02, 0x90, 0x40, 0x05, 0x00, 0x01, 0x00, 0x02};
    auto plug2 = CreateSimpleBlock(0x8109, plug2Primary);

    auto routingBlock = CreateBlockWithNested(0x8108, routingPrimary, {plug0, plug1, plug2});

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(routingBlock.data(), routingBlock.size(), consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetType(), 0x8108);
    
    // Verify we found all 3 SubunitPlugInfo blocks
    auto plugInfoBlocks = result->FindAllNested(0x8109);
    ASSERT_EQ(plugInfoBlocks.size(), 3);
    
    // Verify plug IDs are in order (byte 0 of each primary field)
    EXPECT_EQ(plugInfoBlocks[0].GetPrimaryData()[0], 0x00);
    EXPECT_EQ(plugInfoBlocks[1].GetPrimaryData()[0], 0x01);
    EXPECT_EQ(plugInfoBlocks[2].GetPrimaryData()[0], 0x02);
    
    // The direction logic is tested in MusicSubunit - here we just verify
    // that FindAllNested preserves order, which is critical for position-based direction
}

TEST_F(AVCInfoBlockTests, RoutingStatus_SubunitPlugInfoPrimaryFields) {
    // Verify SubunitPlugInfo (0x8109) primary field structure:
    // [0] = subunit_plug_id
    // [1-2] = fdf_fmt (signal format)
    // [3] = usage/type
    // [4-5] = numClusters
    // [6-7] = numChannels
    
    std::vector<uint8_t> plugPrimary = {
        0x05,        // subunit_plug_id = 5
        0x90, 0x40,  // fdf_fmt (AM824 compound)
        0x04,        // usage = Analog (0x04)
        0x00, 0x02,  // numClusters = 2
        0x00, 0x08   // numChannels = 8
    };

    auto plugBlock = CreateSimpleBlock(0x8109, plugPrimary);

    size_t consumed = 0;
    auto result = AVCInfoBlock::Parse(plugBlock.data(), plugBlock.size(), consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetType(), 0x8109);
    
    const auto& primary = result->GetPrimaryData();
    ASSERT_GE(primary.size(), 8);
    
    EXPECT_EQ(primary[0], 5);    // subunit_plug_id
    EXPECT_EQ(primary[1], 0x90); // fdf_fmt MSB
    EXPECT_EQ(primary[2], 0x40); // fdf_fmt LSB
    EXPECT_EQ(primary[3], 0x04); // usage
    EXPECT_EQ((primary[4] << 8) | primary[5], 2);   // numClusters
    EXPECT_EQ((primary[6] << 8) | primary[7], 8);   // numChannels
}