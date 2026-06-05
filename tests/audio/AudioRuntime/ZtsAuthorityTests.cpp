#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include <gtest/gtest.h>

namespace ASFW::Tests::AudioRuntime {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::ZtsAuthoritySource;
using ASFW::Audio::Runtime::ZtsPublicationMode;
using ASFW::Audio::Runtime::ZtsAuthorityState;

TEST(ZtsAuthorityTests, ResetState) {
    ZtsAuthorityState state{};
    state.selectedSource.store(ZtsAuthoritySource::RxClock);
    state.selectedMode.store(ZtsPublicationMode::DirectToHAL);
    state.sourceGeneration.store(42);
    state.authoritativeSampleFrame.store(100);
    state.authoritativeHostTicks.store(200);
    state.hostNanosPerSampleQ8.store(300);
    state.rxSourceUpdates.store(1);
    state.txSourceUpdates.store(2);
    state.mirrorPublications.store(3);
    state.directPublications.store(4);
    state.rejectedRxUpdates.store(5);
    state.rejectedTxUpdates.store(6);
    state.staleSourceUpdates.store(7);

    state.Reset();

    EXPECT_EQ(state.selectedSource.load(), ZtsAuthoritySource::None);
    EXPECT_EQ(state.selectedMode.load(), ZtsPublicationMode::MirrorPump);
    EXPECT_EQ(state.sourceGeneration.load(), 0U);
    EXPECT_EQ(state.authoritativeSampleFrame.load(), 0U);
    EXPECT_EQ(state.authoritativeHostTicks.load(), 0U);
    EXPECT_EQ(state.hostNanosPerSampleQ8.load(), 0U);
    EXPECT_EQ(state.rxSourceUpdates.load(), 0U);
    EXPECT_EQ(state.txSourceUpdates.load(), 0U);
    EXPECT_EQ(state.mirrorPublications.load(), 0U);
    EXPECT_EQ(state.directPublications.load(), 0U);
    EXPECT_EQ(state.rejectedRxUpdates.load(), 0U);
    EXPECT_EQ(state.rejectedTxUpdates.load(), 0U);
    EXPECT_EQ(state.staleSourceUpdates.load(), 0U);
}

TEST(ZtsAuthorityTests, UpdateAuthoritativeZtsRx) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    // Defaults to None source, so RX update must be rejected
    EXPECT_FALSE(control.UpdateAuthoritativeZtsFromRx(100, 200, 300));
    EXPECT_EQ(control.ztsState.rejectedRxUpdates.load(), 1U);
    EXPECT_EQ(control.ztsState.sourceGeneration.load(), 0U);

    // Set to RxClock source, update should succeed
    control.ztsState.selectedSource.store(ZtsAuthoritySource::RxClock);
    EXPECT_TRUE(control.UpdateAuthoritativeZtsFromRx(100, 200, 300));
    EXPECT_EQ(control.ztsState.authoritativeSampleFrame.load(), 100U);
    EXPECT_EQ(control.ztsState.authoritativeHostTicks.load(), 200U);
    EXPECT_EQ(control.ztsState.hostNanosPerSampleQ8.load(), 300U);
    EXPECT_EQ(control.ztsState.sourceGeneration.load(), 1U);
    EXPECT_EQ(control.ztsState.rxSourceUpdates.load(), 1U);

    // Update with zero ticks or nanos should be rejected as stale/invalid
    EXPECT_FALSE(control.UpdateAuthoritativeZtsFromRx(101, 0, 300));
    EXPECT_EQ(control.ztsState.staleSourceUpdates.load(), 1U);
    EXPECT_EQ(control.ztsState.sourceGeneration.load(), 1U); // Generation doesn't increment
}

TEST(ZtsAuthorityTests, UpdateAuthoritativeZtsTx) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    // Defaults to None source, so TX update must be rejected
    EXPECT_FALSE(control.UpdateAuthoritativeZtsFromTx(100, 200, 300));
    EXPECT_EQ(control.ztsState.rejectedTxUpdates.load(), 1U);

    // Set to TxClock source, update should succeed
    control.ztsState.selectedSource.store(ZtsAuthoritySource::TxClock);
    EXPECT_TRUE(control.UpdateAuthoritativeZtsFromTx(100, 200, 300));
    EXPECT_EQ(control.ztsState.authoritativeSampleFrame.load(), 100U);
    EXPECT_EQ(control.ztsState.authoritativeHostTicks.load(), 200U);
    EXPECT_EQ(control.ztsState.hostNanosPerSampleQ8.load(), 300U);
    EXPECT_EQ(control.ztsState.sourceGeneration.load(), 1U);
    EXPECT_EQ(control.ztsState.txSourceUpdates.load(), 1U);
}

} // namespace ASFW::Tests::AudioRuntime
