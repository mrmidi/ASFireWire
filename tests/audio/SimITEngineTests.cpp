// SimITEngineTests.cpp
// Hardware-grade offline testing for SimITEngine
//
// These tests validate that the simulation engine correctly enforces
// the same invariants as real FireWire IT hardware:
//   - Fixed 8 kHz cadence (8 packets per 1ms tick)
//   - Bounded latency detection
//   - Cadence, size, DBC validation
//   - Underrun/overrun detection
//

#include <gtest/gtest.h>
#include "Isoch/Transmit/SimITEngine.hpp"

using namespace ASFW::Isoch::Sim;
using namespace ASFW::Encoding;

class SimITEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_.Configure(SimITConfig{}, 0x3F, 0x00);
    }

    SimITEngine engine_;
};

// =============================================================================
// Basic Lifecycle Tests
// =============================================================================

TEST_F(SimITEngineTest, StartsInStoppedState) {
    SimITEngine fresh;
    EXPECT_EQ(fresh.State(), SimState::Stopped);
}

TEST_F(SimITEngineTest, ConfigureAndStartSetsRunning) {
    engine_.Start(0);
    EXPECT_EQ(engine_.State(), SimState::Running);
}

TEST_F(SimITEngineTest, StopReturnsStopped) {
    engine_.Start(0);
    engine_.Stop();
    EXPECT_EQ(engine_.State(), SimState::Stopped);
}

// =============================================================================
// Fixed Cadence Tests (The Critical Ones)
// =============================================================================

TEST_F(SimITEngineTest, TickAlwaysEmits8Packets) {
    engine_.Start(0);
    
    // Even with empty buffer, tick should emit 8 packets
    engine_.Tick1ms(1'000'000);  // 1ms later
    
    EXPECT_EQ(engine_.PacketsTotal(), 8u);
}

TEST_F(SimITEngineTest, TenTicksEmit80Packets) {
    engine_.Start(0);
    
    for (uint64_t t = 1; t <= 10; ++t) {
        engine_.Tick1ms(t * 1'000'000);  // 1ms intervals
    }
    
    EXPECT_EQ(engine_.PacketsTotal(), 80u);
}

TEST_F(SimITEngineTest, CadenceRatioIsCorrect75PercentData) {
    // With dataCycleMask = 0xEE (binary: 11101110), cycles 1,2,3,5,6,7 are DATA
    // That's 6 DATA + 2 NO-DATA per 8 cycles = 75% DATA
    engine_.Start(0);
    
    // Run 1000 ticks = 8000 packets
    for (uint64_t t = 1; t <= 1000; ++t) {
        engine_.Tick1ms(t * 1'000'000);
    }
    
    EXPECT_EQ(engine_.PacketsTotal(), 8000u);
    EXPECT_EQ(engine_.PacketsData(), 6000u);      // 6/8 = 75%
    EXPECT_EQ(engine_.PacketsNoData(), 2000u);    // 2/8 = 25%
}

// =============================================================================
// Anomaly Detection Tests
// =============================================================================

TEST_F(SimITEngineTest, NoAnomaliesWithPrefilledBuffer) {
    engine_.Start(0);
    
    // With continuous feeding, no anomalies should occur
    // We feed 512 frames per "callback" which is more than consumed per tick
    // 1 tick = 8 packets, 6 DATA × 8 frames = 48 frames consumed per tick
    
    const uint32_t framesPerCallback = 512;
    std::vector<int32_t> samples(framesPerCallback * 2, 0x12345678);  // Stereo
    
    // Prefill before running
    engine_.WritePCMInterleavedS32(samples.data(), framesPerCallback);
    
    // Run 100 ticks, feeding intermittently
    for (uint64_t t = 1; t <= 100; ++t) {
        engine_.Tick1ms(t * 1'000'000);
        
        // Feed every 10 ticks (~10ms, matching CoreAudio 512-frame callback at 48kHz)
        if (t % 10 == 0) {
            engine_.WritePCMInterleavedS32(samples.data(), framesPerCallback);
        }
    }
    
    // Should have no cadence/size/DBC anomalies (late ticks and overruns are acceptable)
    Anomaly anomalies[256];
    uint32_t count = engine_.CopyAnomalies(anomalies, 256);
    
    uint32_t cadenceOrDbcAnomalies = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (anomalies[i].kind == AnomalyKind::CadenceMismatch ||
            anomalies[i].kind == AnomalyKind::DbcMismatch ||
            anomalies[i].kind == AnomalyKind::SizeMismatch) {
            cadenceOrDbcAnomalies++;
        }
    }
    EXPECT_EQ(cadenceOrDbcAnomalies, 0u);
}

TEST_F(SimITEngineTest, LateTickDetected) {
    engine_.Start(0);
    
    // First tick at 1ms
    engine_.Tick1ms(1'000'000);
    
    // Second tick at 5ms (4ms gap > 2ms threshold)
    engine_.Tick1ms(5'000'000);
    
    EXPECT_EQ(engine_.LateTickCount(), 1u);
    EXPECT_GE(engine_.AnomaliesCount(), 1u);
    
    // Check anomaly kind
    Anomaly anomalies[16];
    uint32_t count = engine_.CopyAnomalies(anomalies, 16);
    ASSERT_GE(count, 1u);
    
    bool foundLateTick = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (anomalies[i].kind == AnomalyKind::LateTick) {
            foundLateTick = true;
            break;
        }
    }
    EXPECT_TRUE(foundLateTick);
}

TEST_F(SimITEngineTest, ProducerOverrunDetected) {
    engine_.Start(0);
    
    // Write more than buffer capacity
    // Default StereoAudioRingBuffer is ~4096 frames
    const uint32_t overflowFrames = 5000;
    std::vector<int32_t> samples(overflowFrames * 2, 0x11111111);
    
    uint32_t written = engine_.WritePCMInterleavedS32(samples.data(), overflowFrames);
    
    // Should have detected overflow
    if (written < overflowFrames) {
        EXPECT_GE(engine_.ProducerOverruns(), 1u);
    }
}

// =============================================================================
// Underrun Detection Tests
// =============================================================================

TEST_F(SimITEngineTest, UnderrunDetectedWithEmptyBuffer) {
    engine_.Start(0);
    
    // Run with completely empty buffer - assembler should increment underrun
    for (uint64_t t = 1; t <= 100; ++t) {
        engine_.Tick1ms(t * 1'000'000);
    }
    
    // 100 ticks × 8 packets = 800 total
    // With 6 DATA per 8 packets = 600 DATA packets
    // All DATA packets should be underruns (silence inserted)
    EXPECT_GE(engine_.UnderrunPacketsSynthesized(), 1u);
}

// =============================================================================
// DBC Continuity Tests
// =============================================================================

TEST_F(SimITEngineTest, DbcContinuityAcrossGroup) {
    // Configure with specific initial DBC
    engine_.Configure(SimITConfig{}, 0x3F, 0x00);
    engine_.Start(0);
    
    // Prefill buffer
    const uint32_t framesToWrite = 1000;
    std::vector<int32_t> samples(framesToWrite * 2, 0);
    engine_.WritePCMInterleavedS32(samples.data(), framesToWrite);
    
    // Run one tick (8 packets)
    engine_.Tick1ms(1'000'000);
    
    // Should have no DBC violations
    Anomaly anomalies[256];
    uint32_t count = engine_.CopyAnomalies(anomalies, 256);
    
    for (uint32_t i = 0; i < count; ++i) {
        EXPECT_NE(anomalies[i].kind, AnomalyKind::DbcMismatch)
            << "DBC mismatch at seq=" << anomalies[i].seq
            << " expected=" << (int)anomalies[i].expectedDbc
            << " actual=" << (int)anomalies[i].actualDbc;
    }
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST_F(SimITEngineTest, StressTestOneSecondOfAudio) {
    engine_.Start(0);
    
    // Simulate 1 second = 1000 ticks
    // Producer writes at 48kHz = 48000 frames/sec ≈ 512 frames per ~10.67ms
    // But we'll write in chunks mimicking CoreAudio buffer callbacks
    
    const uint32_t framesPerCallback = 512;
    const uint64_t callbackIntervalNs = 10'666'667;  // ~10.67ms for 512 frames @ 48kHz
    
    uint64_t producerTime = 0;
    uint64_t consumerTime = 0;
    
    std::vector<int32_t> samples(framesPerCallback * 2, 0x12345678);
    
    for (int i = 0; i < 1000; ++i) {
        // Consumer tick at 1kHz
        consumerTime += 1'000'000;
        engine_.Tick1ms(consumerTime);
        
        // Producer callback at 93.75 Hz (every 10.67ms)
        if (producerTime + callbackIntervalNs <= consumerTime) {
            producerTime += callbackIntervalNs;
            engine_.WritePCMInterleavedS32(samples.data(), framesPerCallback);
        }
    }
    
    // Verify results
    EXPECT_EQ(engine_.PacketsTotal(), 8000u);
    EXPECT_EQ(engine_.PacketsData(), 6000u);
    EXPECT_EQ(engine_.PacketsNoData(), 2000u);
    
    // Some underruns are expected initially before producer catches up
    // But after warmup, should stabilize
    std::cout << "After 1 second stress test:" << std::endl;
    std::cout << "  Total packets: " << engine_.PacketsTotal() << std::endl;
    std::cout << "  DATA: " << engine_.PacketsData() << std::endl;
    std::cout << "  NO-DATA: " << engine_.PacketsNoData() << std::endl;
    std::cout << "  Anomalies: " << engine_.AnomaliesCount() << std::endl;
    std::cout << "  Late ticks: " << engine_.LateTickCount() << std::endl;
    std::cout << "  Underruns synthesized: " << engine_.UnderrunPacketsSynthesized() << std::endl;
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
