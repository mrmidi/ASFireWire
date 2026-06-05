#include <gtest/gtest.h>

#include "../ASFWDriver/AudioEngine/DirectIsoch/Sync/ExternalSyncBridge.hpp"

using ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge;
using ASFW::AudioEngine::DirectIsoch::ExternalSyncClockState;

TEST(ExternalSyncBridge, EstablishesAfterSixteenValidUpdates) {
    ExternalSyncBridge bridge;
    ExternalSyncClockState state;
    bridge.active.store(true, std::memory_order_release);

    for (uint32_t i = 0; i < 15; ++i) {
        uint32_t seq = 0;
        EXPECT_FALSE(state.ObserveSample(bridge,
                                         /*nowHostTicks=*/1000 + i,
                                         /*syt=*/0x1234,
                                         /*fdf=*/ExternalSyncBridge::kFdf48k,
                                         /*dbs=*/6,
                                         &seq));
        EXPECT_EQ(seq, i + 1);
        EXPECT_FALSE(bridge.clockEstablished.load(std::memory_order_acquire));
    }

    uint32_t transitionSeq = 0;
    EXPECT_TRUE(state.ObserveSample(bridge,
                                    /*nowHostTicks=*/2000,
                                    /*syt=*/0x1234,
                                    /*fdf=*/ExternalSyncBridge::kFdf48k,
                                    /*dbs=*/6,
                                    &transitionSeq));
    EXPECT_EQ(transitionSeq, 16u);
    EXPECT_FALSE(bridge.clockEstablished.load(std::memory_order_acquire));
}

TEST(ExternalSyncBridge, ClearsEstablishedOnStaleUpdate) {
    ExternalSyncBridge bridge;
    ExternalSyncClockState state;

    bridge.active.store(true, std::memory_order_release);
    bridge.clockEstablished.store(true, std::memory_order_release);
    bridge.lastUpdateHostTicks.store(100, std::memory_order_release);

    EXPECT_TRUE(state.HandleStale(bridge, /*nowHostTicks=*/250, /*staleThresholdHostTicks=*/100));
    EXPECT_FALSE(bridge.clockEstablished.load(std::memory_order_acquire));
}

TEST(ExternalSyncBridge, TransitionRequiresCallerToFlipEstablishedFlag) {
    ExternalSyncBridge bridge;
    ExternalSyncClockState state;
    bridge.active.store(true, std::memory_order_release);

    for (uint32_t i = 0; i < ExternalSyncClockState::kEstablishValidUpdates; ++i) {
        (void)state.ObserveSample(bridge,
                                  100 + i,
                                  0x2000,
                                  ExternalSyncBridge::kFdf48k,
                                  2,
                                  nullptr);
    }

    EXPECT_FALSE(bridge.clockEstablished.load(std::memory_order_acquire));
    bridge.clockEstablished.store(true, std::memory_order_release);

    EXPECT_FALSE(state.ObserveSample(bridge,
                                     /*nowHostTicks=*/500,
                                     /*syt=*/0x2000,
                                     /*fdf=*/ExternalSyncBridge::kFdf48k,
                                     /*dbs=*/2,
                                     nullptr));
}

TEST(ExternalSyncBridge, CadenceRingCapturesConsecutiveSytDeltas) {
    ExternalSyncBridge bridge;
    ExternalSyncClockState state;
    bridge.active.store(true, std::memory_order_release);

    (void)state.ObserveSample(bridge, 100, 0x79FE, ExternalSyncBridge::kFdf48k, 8, nullptr);
    (void)state.ObserveSample(bridge, 101, 0x91FE, ExternalSyncBridge::kFdf48k, 8, nullptr);

    const auto snapshot = bridge.ReadCadenceSnapshot();
    EXPECT_EQ(snapshot.writeIndex, 1u);
    EXPECT_EQ(snapshot.warmupCount, 1u);
    EXPECT_FALSE(snapshot.established);
    EXPECT_EQ(bridge.ReadCadenceDelta(0), 4096u);
}

TEST(ExternalSyncBridge, CadenceRingWrapDeltaUsesSytOffsetDomain) {
    ExternalSyncBridge bridge;
    ExternalSyncClockState state;
    bridge.active.store(true, std::memory_order_release);

    (void)state.ObserveSample(bridge, 100, 0xFB00, ExternalSyncBridge::kFdf48k, 8, nullptr);
    (void)state.ObserveSample(bridge, 101, 0x1300, ExternalSyncBridge::kFdf48k, 8, nullptr);

    EXPECT_EQ(bridge.ReadCadenceDelta(0), 4096u);
    EXPECT_EQ(bridge.ReadCadenceSnapshot().recoveredDeviceOffsetTicks,
              ASFW::Timing::normalizeOffsetDomain(ASFW::Timing::sytToFieldTicks(0xFB00) + 4096));
}

TEST(ExternalSyncBridge, NoInfoSytDoesNotAdvanceCadence) {
    ExternalSyncBridge bridge;
    ExternalSyncClockState state;
    bridge.active.store(true, std::memory_order_release);

    (void)state.ObserveSample(bridge, 100, 0x79FE, ExternalSyncBridge::kFdf48k, 8, nullptr);
    (void)state.ObserveSample(bridge, 101, ExternalSyncBridge::kNoInfoSyt,
                              ExternalSyncBridge::kFdf48k, 8, nullptr);
    (void)state.ObserveSample(bridge, 102, 0x91FE, ExternalSyncBridge::kFdf48k, 8, nullptr);

    EXPECT_EQ(bridge.ReadCadenceSnapshot().writeIndex, 1u);
    EXPECT_EQ(bridge.ReadCadenceDelta(0), 4096u);
}

TEST(ExternalSyncBridge, InvalidFdfResetsPreviousSytForCadence) {
    ExternalSyncBridge bridge;
    ExternalSyncClockState state;
    bridge.active.store(true, std::memory_order_release);

    (void)state.ObserveSample(bridge, 100, 0x79FE, ExternalSyncBridge::kFdf48k, 8, nullptr);
    EXPECT_FALSE(state.ObserveSample(bridge, 101, 0x91FE, 0x00, 8, nullptr));
    (void)state.ObserveSample(bridge, 102, 0xA1FE, ExternalSyncBridge::kFdf48k, 8, nullptr);

    EXPECT_EQ(bridge.ReadCadenceSnapshot().writeIndex, 0u);
    EXPECT_EQ(bridge.ReadCadenceSnapshot().warmupCount, 0u);
}

TEST(ExternalSyncBridge, CadenceEstablishesAfterFullRingWarmup) {
    ExternalSyncBridge bridge;
    ExternalSyncClockState state;
    bridge.active.store(true, std::memory_order_release);

    uint16_t syt = 0x0000;
    (void)state.ObserveSample(bridge, 100, syt, ExternalSyncBridge::kFdf48k, 8, nullptr);
    for (uint32_t i = 0; i < ExternalSyncBridge::kCadenceRingSize; ++i) {
        syt = static_cast<uint16_t>((syt + 0x1000u) & 0xF000u);
        (void)state.ObserveSample(bridge, 101 + i, syt, ExternalSyncBridge::kFdf48k, 8, nullptr);
    }

    const auto snapshot = bridge.ReadCadenceSnapshot();
    EXPECT_TRUE(snapshot.established);
    EXPECT_EQ(snapshot.warmupCount, ExternalSyncBridge::kCadenceRingSize);
}
