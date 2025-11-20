#include <gtest/gtest.h>
#include "../mocks/MockFireWireBus.hpp"
#include "../mocks/FakeFireWireBus.hpp"
#include "../../ASFWDriver/ConfigROM/ROMReader.hpp"

using namespace ASFW::Discovery;
using namespace ASFW::Async;
using namespace ASFW::Async::Mocks;
using namespace ASFW::Async::Fakes;
using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;

/**
 * @brief Unit tests for ROMReader using mocks (no hardware required).
 *
 * These tests demonstrate how to use MockFireWireBus and FakeFireWireBus
 * to test Discovery layer components without actual FireWire hardware.
 */

// =============================================================================
// MockFireWireBus Tests: Precise Expectations
// =============================================================================

class ROMReaderMockTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up default topology state
        mockBus.SetDefaultTopology(
            Generation{1},
            NodeId{0xFFC0},  // Bus 0, Node 0
            FwSpeed::S400
        );
    }

    MockFireWireBus mockBus;
};

/**
 * Test: ReadBIB succeeds with valid Config ROM header.
 */
TEST_F(ROMReaderMockTest, ReadBIB_Success) {
    // Arrange: Mock returns valid BIB data
    std::vector<uint8_t> validBIB = {
        0x04, 0x04, 0x00, 0x00,  // bus_info_length=4, crc_length=4
        0x31, 0x33, 0x39, 0x34,  // Bus name "1394"
        0x00, 0x00, 0x00, 0x01,  // Node capabilities
        0x00, 0x11, 0x22, 0x33,  // GUID high
        0x44, 0x55, 0x66, 0x77   // GUID low (20 bytes total)
    };

    EXPECT_CALL(mockBus, ReadBlock(
        Generation{1},
        NodeId{0},
        testing::Field(&FWAddress::addressLo, 0xF0000400),  // Config ROM base
        20,  // BIB size
        FwSpeed::S100,  // Always S100 per Apple behavior
        testing::_
    )).WillOnce(Invoke([validBIB](auto, auto, auto, auto, auto, auto callback) {
        callback(AsyncStatus::kSuccess, std::span{validBIB});
        return AsyncHandle{1};
    }));

    // Act: Read BIB
    ROMReader reader(mockBus);
    bool callbackInvoked = false;
    reader.ReadBIB(0, Generation{1}, FwSpeed::S400, [&](const ROMReader::ReadResult& result) {
        // Assert: Callback receives success
        callbackInvoked = true;
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.nodeId, 0);
        EXPECT_EQ(result.generation.value, 1u);
        EXPECT_EQ(result.dataLength, 20u);
        EXPECT_NE(result.data, nullptr);

        // Verify BIB header
        uint32_t header = result.data[0];
        uint8_t bus_info_length = (header >> 24) & 0xFF;
        EXPECT_EQ(bus_info_length, 0x04);
    });

    EXPECT_TRUE(callbackInvoked);
}

/**
 * Test: ReadBIB times out when device doesn't respond.
 */
TEST_F(ROMReaderMockTest, ReadBIB_Timeout) {
    // Arrange: Mock returns timeout
    EXPECT_CALL(mockBus, ReadBlock(_, _, _, _, _, _))
        .WillOnce(Invoke([](auto, auto, auto, auto, auto, auto callback) {
            callback(AsyncStatus::kTimeout, std::span<const uint8_t>{});
            return AsyncHandle{1};
        }));

    // Act: Read BIB
    ROMReader reader(mockBus);
    bool callbackInvoked = false;
    reader.ReadBIB(0, Generation{1}, FwSpeed::S400, [&](const ROMReader::ReadResult& result) {
        // Assert: Callback receives failure
        callbackInvoked = true;
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.nodeId, 0);
    });

    EXPECT_TRUE(callbackInvoked);
}

/**
 * Test: ReadBIB fails when bus reset occurs during read.
 */
TEST_F(ROMReaderMockTest, ReadBIB_BusReset) {
    // Arrange: Mock returns bus reset status
    EXPECT_CALL(mockBus, ReadBlock(_, _, _, _, _, _))
        .WillOnce(Invoke([](auto, auto, auto, auto, auto, auto callback) {
            callback(AsyncStatus::kBusReset, std::span<const uint8_t>{});
            return AsyncHandle{1};
        }));

    // Act: Read BIB
    ROMReader reader(mockBus);
    bool callbackInvoked = false;
    reader.ReadBIB(0, Generation{1}, FwSpeed::S400, [&](const ROMReader::ReadResult& result) {
        // Assert: Callback receives failure
        callbackInvoked = true;
        EXPECT_FALSE(result.success);
    });

    EXPECT_TRUE(callbackInvoked);
}

// =============================================================================
// FakeFireWireBus Tests: Integration-Style Testing
// =============================================================================

class ROMReaderFakeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Program fake Config ROM for node 0
        fakeBus.SetMemory(0, 0xF0000400, {
            // Bus Info Block (20 bytes)
            0x04, 0x04, 0x00, 0x00,  // BIB header
            0x31, 0x33, 0x39, 0x34,  // "1394"
            0x00, 0x00, 0x00, 0x01,  // Capabilities
            0x00, 0x11, 0x22, 0x33,  // GUID high
            0x44, 0x55, 0x66, 0x77,  // GUID low

            // Root directory (32 bytes)
            0x00, 0x06, 0x00, 0x00,  // Directory length=6
            0x03, 0x00, 0x00, 0x01,  // Vendor ID
            0x81, 0x00, 0x00, 0x02,  // Textual descriptor
            0x17, 0x00, 0x00, 0x03,  // Model ID
            0x81, 0x00, 0x00, 0x04,  // Textual descriptor
            0xD1, 0x00, 0x00, 0x05,  // Unit directory
            0x00, 0x00, 0x00, 0x00   // Padding
        });

        fakeBus.SetGeneration(Generation{1});
        fakeBus.SetLocalNodeID(NodeId{0});
        fakeBus.SetSpeed(NodeId{0}, FwSpeed::S400);
    }

    FakeFireWireBus fakeBus;
};

/**
 * Test: ReadBIB returns programmed fake data.
 */
TEST_F(ROMReaderFakeTest, ReadBIB_ReturnsF akeData) {
    ROMReader reader(fakeBus);
    bool callbackInvoked = false;

    reader.ReadBIB(0, Generation{1}, FwSpeed::S400, [&](const ROMReader::ReadResult& result) {
        callbackInvoked = true;
        ASSERT_TRUE(result.success);
        ASSERT_EQ(result.dataLength, 20u);
        ASSERT_NE(result.data, nullptr);

        // Verify BIB header matches fake data
        EXPECT_EQ(result.data[0], 0x04040000u);  // Big-endian header
        EXPECT_EQ(result.data[1], 0x31333934u);  // "1394"
        EXPECT_EQ(result.data[3], 0x00112233u);  // GUID high
        EXPECT_EQ(result.data[4], 0x44556677u);  // GUID low
    });

    EXPECT_TRUE(callbackInvoked);
}

/**
 * Test: ReadBIB times out when address not programmed.
 */
TEST_F(ROMReaderFakeTest, ReadBIB_UnprogrammedAddress_Timeout) {
    FakeFireWireBus emptyBus;  // No memory programmed
    emptyBus.SetGeneration(Generation{1});
    emptyBus.SetLocalNodeID(NodeId{0});

    ROMReader reader(emptyBus);
    bool callbackInvoked = false;

    reader.ReadBIB(0, Generation{1}, FwSpeed::S400, [&](const ROMReader::ReadResult& result) {
        callbackInvoked = true;
        EXPECT_FALSE(result.success);
    });

    EXPECT_TRUE(callbackInvoked);
}

/**
 * Test: ReadBIB detects generation mismatch.
 */
TEST_F(ROMReaderFakeTest, ReadBIB_GenerationMismatch_BusReset) {
    ROMReader reader(fakeBus);
    bool callbackInvoked = false;

    // Try to read with wrong generation
    reader.ReadBIB(0, Generation{99}, FwSpeed::S400, [&](const ROMReader::ReadResult& result) {
        callbackInvoked = true;
        EXPECT_FALSE(result.success);  // Should fail due to generation mismatch
    });

    EXPECT_TRUE(callbackInvoked);
}

// =============================================================================
// Main Test Runner
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
