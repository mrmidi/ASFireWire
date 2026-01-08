// IsochTransmitContextTests.cpp
// ASFW - Unit tests for IsochTransmitContext
//
// Includes two "hard" tests:
// 1. CadenceOrderingTrace - 32 packets exact N-D-D-D sequence
// 2. DBCNoDataBoundary - DBC behavior across NO-DATA boundaries
//

#include <gtest/gtest.h>
#include <array>
#include "../ASFWDriver/Isoch/Transmit/IsochTransmitContext.hpp"

using namespace ASFW::Isoch;
using namespace ASFW::Encoding;

//------------------------------------------------------------------------------
// Basic Lifecycle Tests
//------------------------------------------------------------------------------

TEST(IsochTransmitContext, InitialStateIsUnconfigured) {
    IsochTransmitContext ctx;
    EXPECT_EQ(ctx.GetState(), ITState::Unconfigured);
}

TEST(IsochTransmitContext, ConfigureTransitionsToConfigured) {
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    EXPECT_EQ(ctx.GetState(), ITState::Configured);
}

TEST(IsochTransmitContext, StartTransitionsToRunning) {
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    ctx.Start();
    EXPECT_EQ(ctx.GetState(), ITState::Running);
}

TEST(IsochTransmitContext, StopTransitionsToStopped) {
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    ctx.Start();
    ctx.Stop();
    EXPECT_EQ(ctx.GetState(), ITState::Stopped);
}

TEST(IsochTransmitContext, PollDoesNothingWhenNotRunning) {
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    ctx.Poll();  // Not started yet
    EXPECT_EQ(ctx.PacketsAssembled(), 0);
}

//------------------------------------------------------------------------------
// Statistics Tests
//------------------------------------------------------------------------------

TEST(IsochTransmitContext, PollProcesses8PacketsPerTick) {
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    ctx.Start();
    
    ctx.Poll();  // 8 packets
    EXPECT_EQ(ctx.PacketsAssembled(), 8);
    
    ctx.Poll();  // 8 more
    EXPECT_EQ(ctx.PacketsAssembled(), 16);
}

TEST(IsochTransmitContext, OneSecondProduces8000Packets) {
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    ctx.Start();
    
    // 1000 ticks × 8 packets = 8000 packets/second
    for (int i = 0; i < 1000; ++i) {
        ctx.Poll();
    }
    
    EXPECT_EQ(ctx.PacketsAssembled(), 8000);
    EXPECT_EQ(ctx.TickCount(), 1000);
}

TEST(IsochTransmitContext, CadenceRatioIs6to2) {
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    ctx.Start();
    
    // Run for 1 second (1000 ticks)
    for (int i = 0; i < 1000; ++i) {
        ctx.Poll();
    }
    
    // 8000 packets total: 6000 DATA + 2000 NO-DATA
    EXPECT_EQ(ctx.DataPackets(), 6000);
    EXPECT_EQ(ctx.NoDataPackets(), 2000);
}

//------------------------------------------------------------------------------
// HARD TEST 1: Ordering-Sensitive Cadence Trace (32 packets)
//------------------------------------------------------------------------------

TEST(IsochTransmitContext, CadenceOrderingTrace32Packets) {
    // Verify exact sequence: N-D-D-D-N-D-D-D repeated 4 times
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    ctx.Start();
    
    // Expected pattern (4 complete 8-cycle groups = 32 packets)
    // N-D-D-D-N-D-D-D (positions 0,4 are NO-DATA)
    std::array<bool, 32> expectedIsData = {
        false, true, true, true, false, true, true, true,  // Group 0
        false, true, true, true, false, true, true, true,  // Group 1
        false, true, true, true, false, true, true, true,  // Group 2
        false, true, true, true, false, true, true, true   // Group 3
    };
    
    // We need to call Poll() 4 times (4 × 8 = 32 packets)
    // But we can't get individual packets from Poll(), so use PacketAssembler directly
    PacketAssembler assembler(0x3F);
    
    for (int i = 0; i < 32; ++i) {
        auto pkt = assembler.assembleNext(0xFFFF);
        EXPECT_EQ(pkt.isData, expectedIsData[i]) 
            << "Packet " << i << " expected " << (expectedIsData[i] ? "DATA" : "NO-DATA")
            << " but got " << (pkt.isData ? "DATA" : "NO-DATA");
    }
}

//------------------------------------------------------------------------------
// HARD TEST 2: DBC Behavior Across NO-DATA Boundaries
//------------------------------------------------------------------------------

TEST(IsochTransmitContext, DBCNoDataBoundary) {
    // Per IEC 61883-1 blocking mode:
    // - NO-DATA carries the DBC of the NEXT DATA packet
    // - Next DATA packet uses the same DBC value
    // - DATA increments DBC by sample count (8)
    
    PacketAssembler assembler(0x3F);
    
    // Cycle 0: NO-DATA (should have DBC of next DATA)
    auto pkt0 = assembler.assembleNext(0xFFFF);
    EXPECT_FALSE(pkt0.isData);
    
    // Cycle 1: DATA (should have same DBC as previous NO-DATA)
    auto pkt1 = assembler.assembleNext(0x1234);
    EXPECT_TRUE(pkt1.isData);
    EXPECT_EQ(pkt0.dbc, pkt1.dbc) << "NO-DATA[0] should share DBC with DATA[1]";
    
    // Cycle 2: DATA (DBC increments by 8)
    auto pkt2 = assembler.assembleNext(0x1234);
    EXPECT_TRUE(pkt2.isData);
    EXPECT_EQ(pkt2.dbc, (pkt1.dbc + 8) & 0xFF) << "DATA[2] should be DATA[1] + 8";
    
    // Cycle 3: DATA (DBC increments by 8)
    auto pkt3 = assembler.assembleNext(0x1234);
    EXPECT_TRUE(pkt3.isData);
    EXPECT_EQ(pkt3.dbc, (pkt2.dbc + 8) & 0xFF);
    
    // Cycle 4: NO-DATA (should have DBC of next DATA)
    auto pkt4 = assembler.assembleNext(0xFFFF);
    EXPECT_FALSE(pkt4.isData);
    
    // Cycle 5: DATA (should have same DBC as previous NO-DATA)
    auto pkt5 = assembler.assembleNext(0x1234);
    EXPECT_TRUE(pkt5.isData);
    EXPECT_EQ(pkt4.dbc, pkt5.dbc) << "NO-DATA[4] should share DBC with DATA[5]";
    EXPECT_EQ(pkt5.dbc, (pkt3.dbc + 8) & 0xFF) << "DATA[5] should be DATA[3] + 8";
}

//------------------------------------------------------------------------------
// DBC Continuity Over Long Sequence
//------------------------------------------------------------------------------

// TODO: Add currentDbc() accessor to PacketAssembler to enable this test
// TEST(IsochTransmitContext, DBCContinuityOver256) { ... }

//------------------------------------------------------------------------------
// Underrun Handling
//------------------------------------------------------------------------------

TEST(IsochTransmitContext, UnderrunCountsOnEmptyBuffer) {
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    ctx.Start();
    
    // Don't write any audio to ring buffer
    // Poll should produce silence packets and count underruns
    ctx.Poll();
    
    // Should have had underruns for DATA packets (6 out of 8)
    EXPECT_GT(ctx.UnderrunCount(), 0);
}

TEST(IsochTransmitContext, NoUnderrunsWithPrefilledBuffer) {
    IsochTransmitContext ctx;
    ctx.Configure(0, 0x3F);
    ctx.Start();
    
    // Prefill ring buffer with audio (AFTER Start() to avoid reset)
    std::array<int32_t, 512 * 2> audioData{};
    for (size_t i = 0; i < audioData.size(); ++i) {
        audioData[i] = static_cast<int32_t>(i);
    }
    ctx.RingBuffer().write(audioData.data(), 512);
    
    // Poll should consume from buffer without underruns
    ctx.Poll();
    
    // 8 packets: 6 DATA consume 48 frames (6 × 8), we provided 512
    EXPECT_EQ(ctx.UnderrunCount(), 0);
    EXPECT_EQ(ctx.BufferFillLevel(), 512 - 48);
}
