// AudioRingBufferTests.cpp
// ASFW - Phase 1.5 Encoding Tests
//
// Tests for lock-free SPSC audio ring buffer.
//

#include <gtest/gtest.h>
#include "Isoch/Encoding/AudioRingBuffer.hpp"
#include <thread>
#include <vector>

using namespace ASFW::Encoding;

// Use smaller buffer for faster tests
using TestRingBuffer = AudioRingBuffer<64, 2>;

//==============================================================================
// Initial State Tests
//==============================================================================

TEST(AudioRingBufferTests, InitiallyEmpty) {
    TestRingBuffer buffer;
    EXPECT_TRUE(buffer.isEmpty());
    EXPECT_FALSE(buffer.isFull());
    EXPECT_EQ(buffer.fillLevel(), 0);
}

TEST(AudioRingBufferTests, CorrectCapacity) {
    TestRingBuffer buffer;
    // Capacity is FrameCount - 1 (one slot reserved)
    EXPECT_EQ(buffer.capacity(), 63);
}

TEST(AudioRingBufferTests, InitialCountersZero) {
    TestRingBuffer buffer;
    EXPECT_EQ(buffer.underrunCount(), 0);
    EXPECT_EQ(buffer.overflowCount(), 0);
}

//==============================================================================
// Basic Write/Read Tests
//==============================================================================

TEST(AudioRingBufferTests, WriteAndRead) {
    TestRingBuffer buffer;
    
    // Write some frames
    int32_t writeData[] = {100, 200, 300, 400};  // 2 stereo frames
    uint32_t written = buffer.write(writeData, 2);
    EXPECT_EQ(written, 2);
    EXPECT_EQ(buffer.fillLevel(), 2);
    
    // Read them back
    int32_t readData[4] = {};
    uint32_t read = buffer.read(readData, 2);
    EXPECT_EQ(read, 2);
    EXPECT_EQ(buffer.fillLevel(), 0);
    
    // Verify data
    EXPECT_EQ(readData[0], 100);
    EXPECT_EQ(readData[1], 200);
    EXPECT_EQ(readData[2], 300);
    EXPECT_EQ(readData[3], 400);
}

TEST(AudioRingBufferTests, PartialRead) {
    TestRingBuffer buffer;
    
    // Write 4 frames
    int32_t writeData[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    buffer.write(writeData, 4);
    
    // Read only 2
    int32_t readData[4] = {};
    uint32_t read = buffer.read(readData, 2);
    EXPECT_EQ(read, 2);
    EXPECT_EQ(buffer.fillLevel(), 2);
    
    // Verify first 2 frames were read
    EXPECT_EQ(readData[0], 1);
    EXPECT_EQ(readData[1], 2);
    EXPECT_EQ(readData[2], 3);
    EXPECT_EQ(readData[3], 4);
}

TEST(AudioRingBufferTests, MultipleWritesAndReads) {
    TestRingBuffer buffer;
    
    for (int batch = 0; batch < 10; ++batch) {
        // Write 8 frames
        int32_t writeData[16];
        for (int i = 0; i < 16; ++i) {
            writeData[i] = batch * 100 + i;
        }
        buffer.write(writeData, 8);
        
        // Read 8 frames
        int32_t readData[16] = {};
        buffer.read(readData, 8);
        
        // Verify
        for (int i = 0; i < 16; ++i) {
            EXPECT_EQ(readData[i], batch * 100 + i);
        }
    }
}

//==============================================================================
// Wraparound Tests
//==============================================================================

TEST(AudioRingBufferTests, WrapsAroundCorrectly) {
    TestRingBuffer buffer;
    
    // Fill to near capacity, read, and write again to trigger wrap
    int32_t data[120];  // 60 frames
    for (int i = 0; i < 120; ++i) data[i] = i;
    
    buffer.write(data, 50);
    EXPECT_EQ(buffer.fillLevel(), 50);
    
    buffer.read(data, 50);
    EXPECT_EQ(buffer.fillLevel(), 0);
    
    // Now write again - this should wrap around
    for (int i = 0; i < 80; ++i) data[i] = 1000 + i;
    buffer.write(data, 40);
    EXPECT_EQ(buffer.fillLevel(), 40);
    
    int32_t readBack[80] = {};
    buffer.read(readBack, 40);
    
    // Verify wraparound data is correct
    for (int i = 0; i < 80; ++i) {
        EXPECT_EQ(readBack[i], 1000 + i) << "Mismatch at index " << i;
    }
}

//==============================================================================
// Underrun/Overflow Tests
//==============================================================================

TEST(AudioRingBufferTests, DetectsUnderrun) {
    TestRingBuffer buffer;
    
    // Try to read from empty buffer
    int32_t data[4] = {-1, -1, -1, -1};
    uint32_t read = buffer.read(data, 2);
    
    EXPECT_EQ(read, 0);
    EXPECT_EQ(buffer.underrunCount(), 1);
    
    // Verify silence was written
    EXPECT_EQ(data[0], 0);
    EXPECT_EQ(data[1], 0);
    EXPECT_EQ(data[2], 0);
    EXPECT_EQ(data[3], 0);
}

TEST(AudioRingBufferTests, PartialUnderrunFillsSilence) {
    TestRingBuffer buffer;
    
    // Write only 2 frames
    int32_t writeData[] = {100, 200, 300, 400};  // 2 stereo frames
    buffer.write(writeData, 2);
    
    // Request 4 frames (only 2 available)
    int32_t readData[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t read = buffer.read(readData, 4);
    
    EXPECT_EQ(read, 2);  // Only 2 frames returned
    EXPECT_EQ(buffer.underrunCount(), 1);  // Partial underrun counted
    
    // First 2 frames have data
    EXPECT_EQ(readData[0], 100);
    EXPECT_EQ(readData[1], 200);
    EXPECT_EQ(readData[2], 300);
    EXPECT_EQ(readData[3], 400);
    
    // Remaining 2 frames filled with silence
    EXPECT_EQ(readData[4], 0);
    EXPECT_EQ(readData[5], 0);
    EXPECT_EQ(readData[6], 0);
    EXPECT_EQ(readData[7], 0);
}

TEST(AudioRingBufferTests, DetectsOverflow) {
    TestRingBuffer buffer;
    
    // Fill the buffer completely
    std::vector<int32_t> bigData(128, 42);  // 64 frames
    buffer.write(bigData.data(), 63);  // Max capacity
    
    EXPECT_TRUE(buffer.isFull());
    
    // Try to write more
    int32_t extra[2] = {999, 999};
    uint32_t written = buffer.write(extra, 1);
    
    EXPECT_EQ(written, 0);
    EXPECT_EQ(buffer.overflowCount(), 1);
}

TEST(AudioRingBufferTests, UnderrunCountAccumulates) {
    TestRingBuffer buffer;
    
    int32_t data[4];
    buffer.read(data, 2);
    buffer.read(data, 2);
    buffer.read(data, 2);
    
    EXPECT_EQ(buffer.underrunCount(), 3);
}

//==============================================================================
// Reset Tests
//==============================================================================

TEST(AudioRingBufferTests, ResetClearsAll) {
    TestRingBuffer buffer;
    
    // Write some data
    int32_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    buffer.write(data, 4);
    
    // Read all data first
    buffer.read(data, 4);
    
    // Now read from empty buffer to trigger underrun
    buffer.read(data, 2);
    
    EXPECT_GT(buffer.underrunCount(), 0);
    
    // Reset
    buffer.reset();
    
    EXPECT_TRUE(buffer.isEmpty());
    EXPECT_EQ(buffer.fillLevel(), 0);
    EXPECT_EQ(buffer.underrunCount(), 0);
    EXPECT_EQ(buffer.overflowCount(), 0);
}

//==============================================================================
// Edge Cases
//==============================================================================

TEST(AudioRingBufferTests, ZeroFrameWrite) {
    TestRingBuffer buffer;
    
    int32_t data[4] = {1, 2, 3, 4};
    uint32_t written = buffer.write(data, 0);
    
    EXPECT_EQ(written, 0);
    EXPECT_TRUE(buffer.isEmpty());
}

TEST(AudioRingBufferTests, ZeroFrameRead) {
    TestRingBuffer buffer;
    
    int32_t data[4] = {-1, -1, -1, -1};
    uint32_t read = buffer.read(data, 0);
    
    EXPECT_EQ(read, 0);
    EXPECT_EQ(buffer.underrunCount(), 0);  // Should not count as underrun
}

TEST(AudioRingBufferTests, ExactCapacityFill) {
    TestRingBuffer buffer;
    
    // Fill to exact capacity (63 frames)
    std::vector<int32_t> data(126, 42);  // 63 frames * 2 channels
    uint32_t written = buffer.write(data.data(), 63);
    
    EXPECT_EQ(written, 63);
    EXPECT_TRUE(buffer.isFull());
    EXPECT_EQ(buffer.availableSpace(), 0);
}

//==============================================================================
// Default Buffer Type Tests
//==============================================================================

TEST(AudioRingBufferTests, DefaultStereoBuffer) {
    StereoAudioRingBuffer buffer;
    
    // Should have 4095 frame capacity (4096 - 1)
    EXPECT_EQ(buffer.capacity(), 4095);
}
