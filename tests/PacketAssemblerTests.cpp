// PacketAssemblerTests.cpp
// ASFW - Phase 1.5 Encoding Tests
//
// Integration tests for PacketAssembler using FireBug capture data.
// Reference: 000-48kORIG.txt
//

#include <gtest/gtest.h>
#include "Isoch/Encoding/PacketAssembler.hpp"

using namespace ASFW::Encoding;

//==============================================================================
// Initial State Tests
//==============================================================================

TEST(PacketAssemblerTests, InitialState) {
    PacketAssembler assembler(0x02);  // SID = 2
    
    EXPECT_EQ(assembler.currentCycle(), 0);
    EXPECT_EQ(assembler.bufferFillLevel(), 0);
    EXPECT_EQ(assembler.underrunCount(), 0);
}

TEST(PacketAssemblerTests, FirstPacketIsNoData) {
    PacketAssembler assembler(0x02);
    
    // First cycle in pattern is NO-DATA
    EXPECT_FALSE(assembler.nextIsData());
}

//==============================================================================
// Cadence Pattern Tests
//==============================================================================

TEST(PacketAssemblerTests, FollowsNDDDPattern) {
    PacketAssembler assembler(0x02);
    
    // Pattern: N-D-D-D repeating
    bool expected[] = {false, true, true, true, false, true, true, true};
    
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(i));
        
        AssembledPacket pkt = assembler.assembleNext(0);
        EXPECT_EQ(pkt.isData, expected[i]);
        EXPECT_EQ(pkt.cycleNumber, static_cast<uint64_t>(i));
    }
}

TEST(PacketAssemblerTests, CorrectPacketSizes) {
    PacketAssembler assembler(0x02);
    
    // Expected sizes: 8, 72, 72, 72, 8, 72, 72, 72
    uint32_t expectedSizes[] = {8, 72, 72, 72, 8, 72, 72, 72};
    
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(i));
        
        AssembledPacket pkt = assembler.assembleNext(0);
        EXPECT_EQ(pkt.size, expectedSizes[i]);
    }
}

//==============================================================================
// DBC Sequence Tests (verified against FireBug capture)
//==============================================================================

TEST(PacketAssemblerTests, DBCSequenceMatchesCapture) {
    PacketAssembler assembler(0x02);
    assembler.reset(0xC0);  // Start at DBC=0xC0 like capture
    
    // Expected DBC from 000-48kORIG.txt cycles 977-984:
    // C0, C0, C8, D0, D8, D8, E0, E8
    uint8_t expectedDbc[] = {0xC0, 0xC0, 0xC8, 0xD0, 0xD8, 0xD8, 0xE0, 0xE8};
    
    for (int i = 0; i < 8; i++) {
        SCOPED_TRACE("Cycle " + std::to_string(i));
        
        AssembledPacket pkt = assembler.assembleNext(0);
        EXPECT_EQ(pkt.dbc, expectedDbc[i]);
    }
}

//==============================================================================
// NO-DATA Packet Tests
//==============================================================================

TEST(PacketAssemblerTests, NoDataPacketFormat) {
    PacketAssembler assembler(0x02);
    
    // First packet is NO-DATA
    AssembledPacket pkt = assembler.assembleNext(0);
    
    EXPECT_FALSE(pkt.isData);
    EXPECT_EQ(pkt.size, 8);
    
    // Verify CIP header in BIG-ENDIAN wire order (as it appears on FireWire)
    // Q0: [SID][DBS][rsv/SPH/QPC/FN][DBC] = 0x02020000
    // Bytes: [0]=0x02 (SID), [1]=0x02 (DBS), [2]=0x00, [3]=0x00 (DBC)
    EXPECT_EQ(pkt.data[0], 0x02);  // SID
    EXPECT_EQ(pkt.data[1], 0x02);  // DBS
    EXPECT_EQ(pkt.data[2], 0x00);  // FN/QPC/SPH/rsv
    EXPECT_EQ(pkt.data[3], 0x00);  // DBC (initial = 0)
    
    // Q1: [EOH|FMT][FDF][SYT_high][SYT_low] = 0x9002FFFF
    // Bytes: [4]=0x90 (EOH=10,FMT=0x10), [5]=0x02 (FDF), [6]=0xFF, [7]=0xFF
    EXPECT_EQ(pkt.data[4], 0x90);  // EOH=10 | FMT=0x10
    EXPECT_EQ(pkt.data[5], 0x02);  // FDF (SFC=0x02 for 48kHz)
    EXPECT_EQ(pkt.data[6], 0xFF);  // SYT high byte
    EXPECT_EQ(pkt.data[7], 0xFF);  // SYT low byte
}

//==============================================================================
// DATA Packet Tests
//==============================================================================

TEST(PacketAssemblerTests, DataPacketFormat) {
    PacketAssembler assembler(0x02);
    
    // Skip first NO-DATA packet
    assembler.assembleNext(0);
    
    // Second packet is DATA
    AssembledPacket pkt = assembler.assembleNext(0x79FE);
    
    EXPECT_TRUE(pkt.isData);
    EXPECT_EQ(pkt.size, 72);
    
    // Verify has CIP header (8 bytes) + audio data (64 bytes)
    // Audio should be silence (underrun from empty buffer)
}

TEST(PacketAssemblerTests, DataPacketWithAudio) {
    PacketAssembler assembler(0x02);
    
    // Write some audio to the ring buffer
    int32_t samples[16];  // 8 stereo frames
    for (int i = 0; i < 16; i++) {
        samples[i] = (i + 1) << 8;  // 24-bit values in upper bits
    }
    assembler.ringBuffer().write(samples, 8);
    
    EXPECT_EQ(assembler.bufferFillLevel(), 8);
    
    // Skip first NO-DATA packet
    assembler.assembleNext(0);
    
    // Second packet is DATA with audio
    AssembledPacket pkt = assembler.assembleNext(0);
    
    EXPECT_TRUE(pkt.isData);
    EXPECT_EQ(pkt.size, 72);
    
    // Buffer should be drained
    EXPECT_EQ(assembler.bufferFillLevel(), 0);
    EXPECT_EQ(assembler.underrunCount(), 0);
}

//==============================================================================
// Underrun Handling Tests
//==============================================================================

TEST(PacketAssemblerTests, HandlesUnderrun) {
    PacketAssembler assembler(0x02);
    
    // Skip NO-DATA
    assembler.assembleNext(0);
    
    // Assemble DATA with empty buffer → underrun
    AssembledPacket pkt = assembler.assembleNext(0);
    
    EXPECT_TRUE(pkt.isData);
    EXPECT_EQ(pkt.size, 72);  // Still produces valid packet
    EXPECT_GT(assembler.underrunCount(), 0);
    
    // Audio data should be silence (AM824 encoded zeros)
    // After CIP header (8 bytes), first audio quadlet
    uint32_t* firstQuadlet = reinterpret_cast<uint32_t*>(pkt.data + 8);
    // Silence = 0x40000000 → byte swapped = 0x00000040
    EXPECT_EQ(*firstQuadlet, 0x00000040);
}

//==============================================================================
// Full Cycle Sequence Tests
//==============================================================================

TEST(PacketAssemblerTests, Full8CycleSequence) {
    PacketAssembler assembler(0x02);
    
    // Fill buffer with enough samples for 6 DATA packets
    // 6 DATA × 8 samples = 48 stereo frames
    int32_t samples[96];  // 48 frames × 2 channels
    for (int i = 0; i < 96; i++) {
        samples[i] = (i * 100) << 8;
    }
    assembler.ringBuffer().write(samples, 48);
    
    EXPECT_EQ(assembler.bufferFillLevel(), 48);
    
    // Assemble 8 packets (6 DATA + 2 NO-DATA)
    uint32_t totalSamples = 0;
    
    for (int i = 0; i < 8; i++) {
        AssembledPacket pkt = assembler.assembleNext(0);
        
        if (pkt.isData) {
            totalSamples += kSamplesPerDataPacket;
        }
    }
    
    // Should have consumed 48 samples
    EXPECT_EQ(totalSamples, 48);
    EXPECT_EQ(assembler.bufferFillLevel(), 0);
    EXPECT_EQ(assembler.underrunCount(), 0);
}

//==============================================================================
// Reset Tests
//==============================================================================

TEST(PacketAssemblerTests, ResetClearsAll) {
    PacketAssembler assembler(0x02);
    
    // Advance some cycles
    for (int i = 0; i < 10; i++) {
        assembler.assembleNext(0);
    }
    
    EXPECT_GT(assembler.currentCycle(), 0);
    EXPECT_GT(assembler.underrunCount(), 0);  // Had underruns
    
    assembler.reset();
    
    EXPECT_EQ(assembler.currentCycle(), 0);
    EXPECT_EQ(assembler.underrunCount(), 0);
    EXPECT_FALSE(assembler.nextIsData());  // Back to first cycle (NO-DATA)
}

TEST(PacketAssemblerTests, ResetWithInitialDBC) {
    PacketAssembler assembler(0x02);
    
    assembler.reset(0xC0);
    
    AssembledPacket pkt = assembler.assembleNext(0);
    EXPECT_EQ(pkt.dbc, 0xC0);
}

//==============================================================================
// Sample Rate Verification
//==============================================================================

TEST(PacketAssemblerTests, Produces48kSamplesPerSecond) {
    PacketAssembler assembler(0x02);
    
    // Fill with plenty of samples
    std::vector<int32_t> samples(10000, 0);
    assembler.ringBuffer().write(samples.data(), 5000);
    
    // Simulate 8000 cycles (1 second at FireWire rate)
    uint32_t totalSamples = 0;
    
    for (int i = 0; i < 8000; i++) {
        AssembledPacket pkt = assembler.assembleNext(0);
        if (pkt.isData) {
            totalSamples += kSamplesPerDataPacket;
        }
    }
    
    // Should be exactly 48000 samples (48 kHz)
    // 6 DATA per 8 cycles × 8 samples = 48 per 8 cycles
    // 48 × 1000 = 48000
    EXPECT_EQ(totalSamples, 48000);
}
