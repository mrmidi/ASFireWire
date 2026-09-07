// SPDX-License-Identifier: Apache-2.0
#include "Audio/Shared/AudioRuntimeTuningStore.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace {
using namespace ASFW::Audio::Shared;
constexpr uint32_t depth = static_cast<uint32_t>(TuningGroup::kTransmitDepth);
AudioRuntimeTuning Duet() {
    AudioRuntimeTuning t{};
    t.outputLatencyFrames = 67; t.inputLatencyFrames = 40;
    t.outputSafetyOffsetFrames = t.inputSafetyOffsetFrames = 50;
    return t;
}
RuntimeTuningState Ready() {
    RuntimeTuningState state;
    state.PublishGraph(Duet(), 48000, 2, 2);
    return state;
}
TEST(AudioRuntimeTuningTransaction, ReadbackRequiresAnInstalledGraph) {
    RuntimeTuningState state;
    EXPECT_FALSE(state.Copy().ready);
    EXPECT_EQ(state.Submit(Duet(), depth).error(), TuningRejection::kNotReady);
    state.PublishGraph(Duet(), 96000, 2, 4);
    const auto s = state.Copy();
    EXPECT_TRUE(s.ready);
    EXPECT_EQ(s.effective.outputLatencyFrames, 67U);
    EXPECT_EQ(s.effective.inputLatencyFrames, 40U);
    EXPECT_EQ(s.effective.outputSafetyOffsetFrames, 50U);
    EXPECT_EQ(s.outputChannels, 4U);
}
TEST(AudioRuntimeTuningTransaction, RejectsUnsupportedAndUnknownGroups) {
    auto state = Ready();
    for (uint32_t mask : {2U, 4U, 7U, 0x80000000U}) {
        EXPECT_EQ(state.Submit(Duet(), mask).error(), TuningRejection::kUnsupportedGroups);
        EXPECT_EQ(state.Copy().pendingGroups, 0U);
        EXPECT_EQ(state.Copy().appliedSequence, 0U);
    }
}
TEST(AudioRuntimeTuningTransaction, SelectedDepthIgnoresMalformedUnselectedFields) {
    auto state = Ready();
    auto request = Duet();
    request.txDispatchSlackPackets = 24;
    request.outputLatencyFrames = UINT32_MAX;
    request.frameRingFrames = 0;
    auto id = state.Submit(request, depth);
    ASSERT_TRUE(id);
    ASSERT_TRUE(state.RequestWindow(*id));
    const auto candidate = state.BeginApply(*id);
    ASSERT_TRUE(candidate);
    EXPECT_EQ(candidate->outputLatencyFrames, 67U);
    EXPECT_EQ(candidate->frameRingFrames, Duet().frameRingFrames);
    EXPECT_EQ(state.Copy().effective.txDispatchSlackPackets,
              AudioTimingGeometry::kTxDispatchSlackCycleSlots);
    ASSERT_TRUE(state.Finish(*id, 0));
    EXPECT_EQ(state.Copy().effective.txDispatchSlackPackets, 24U);
    EXPECT_EQ(state.Copy().status, TuningRequestStatus::Applied);
}
TEST(AudioRuntimeTuningTransaction, NoOpDoesNotAllocateAWindow) {
    auto state = Ready();
    auto request = Duet(); request.txDispatchSlackPackets = 24;
    EXPECT_EQ(*state.Submit(request, 0), 0U);
    EXPECT_EQ(*state.Submit(Duet(), depth), 0U);
    EXPECT_EQ(state.Copy().pendingGroups, 0U);
    EXPECT_EQ(state.Copy().requestId, 0U);
    EXPECT_FALSE(state.RequestWindow(0));
}
TEST(AudioRuntimeTuningTransaction, BusyRequestCannotOverwriteOrConsumeExistingRequest) {
    auto state = Ready();
    auto a = Duet(); a.txDispatchSlackPackets = 48;
    auto b = Duet(); b.txDispatchSlackPackets = 12;
    const auto id = state.Submit(a, depth); ASSERT_TRUE(id);
    EXPECT_EQ(state.Submit(b, depth).error(), TuningRejection::kBusy);
    EXPECT_FALSE(state.Finish(*id, 0));
    ASSERT_TRUE(state.RequestWindow(*id));
    EXPECT_FALSE(state.RequestWindow(*id));
    EXPECT_FALSE(state.BeginApply(*id + 1));
    const auto candidate = state.BeginApply(*id); ASSERT_TRUE(candidate);
    EXPECT_EQ(candidate->txDispatchSlackPackets, 48U);
    EXPECT_FALSE(state.BeginApply(*id));
    ASSERT_TRUE(state.Finish(*id, 0));
    EXPECT_FALSE(state.Finish(*id, 0));
    EXPECT_EQ(state.Copy().appliedSequence, 1U);
}
TEST(AudioRuntimeTuningTransaction, RefusalAndAbortHaveTerminalOutcomesAndStaleCallbacksAreHarmless) {
    for (bool aborted : {false, true}) {
        auto state = Ready(); auto request = Duet(); request.txDispatchSlackPackets = 24;
        const auto a = state.Submit(request, depth); ASSERT_TRUE(a);
        ASSERT_TRUE(state.RequestWindow(*a));
        ASSERT_TRUE(state.Finish(*a, 123, aborted));
        EXPECT_EQ(state.Copy().lastError, 123U);
        EXPECT_EQ(state.Copy().status, aborted ? TuningRequestStatus::Aborted : TuningRequestStatus::Rejected);
        EXPECT_EQ(state.Copy().pendingGroups, 0U);
        EXPECT_EQ(state.Copy().effective.txDispatchSlackPackets,
              AudioTimingGeometry::kTxDispatchSlackCycleSlots);
        const auto b = state.Submit(request, depth); ASSERT_TRUE(b); EXPECT_NE(*a, *b);
        EXPECT_FALSE(state.RequestWindow(*a));
        EXPECT_FALSE(state.BeginApply(*a));
        EXPECT_FALSE(state.Finish(*a, 456, true));
        ASSERT_TRUE(state.RequestWindow(*b)); ASSERT_TRUE(state.BeginApply(*b));
        ASSERT_TRUE(state.Finish(*b, 0));
        EXPECT_EQ(state.Copy().appliedSequence, 1U);
    }
}
TEST(AudioRuntimeTuningTransaction, DisconnectCancelsAndDoesNotReuseRequestIds) {
    auto state = Ready(); auto request = Duet(); request.txDispatchSlackPackets = 24;
    const auto a = state.Submit(request, depth); ASSERT_TRUE(a);
    state.Disconnect(99);
    EXPECT_FALSE(state.Copy().ready);
    EXPECT_EQ(state.Copy().status, TuningRequestStatus::Aborted);
    EXPECT_FALSE(state.RequestWindow(*a));
    state.PublishGraph(Duet(), 192000, 2, 2);
    const auto b = state.Submit(request, depth); ASSERT_TRUE(b); EXPECT_GT(*b, *a);
    EXPECT_FALSE(state.Finish(*a, 0));
}
TEST(AudioRuntimeTuningTransaction, StoreProvidesCoherentConcurrentSnapshots) {
    RuntimeTuningStore store;
    std::atomic<bool> done{false};
    std::atomic<uint32_t> bad{0};
    store.WithLock([](auto& state) { state.PublishGraph(Duet(), 67, 67, 67); });
    std::thread writer([&] {
        for (uint32_t i = 1; i <= 100000; ++i) {
            auto t = Duet(); t.outputLatencyFrames = i;
            store.WithLock([&](auto& state) { state.PublishGraph(t, i, i, i); });
        }
        done.store(true);
    });
    do {
        const auto s = store.WithLock([](auto& state) { return state.Copy(); });
        if (s.effective.outputLatencyFrames != s.sampleRateHz ||
            s.inputChannels != s.sampleRateHz || s.outputChannels != s.sampleRateHz) ++bad;
    } while (!done.load());
    writer.join(); EXPECT_EQ(bad.load(), 0U);
}
TEST(AudioRuntimeTuningTransaction, OverflowCannotTurnExcessiveSlackIntoAnAcceptedTarget) {
    // One packet past what the shared store can hold, plus values chosen so a
    // naive `slack + 2 * guard` would wrap. Derived, because the first entry is
    // the store bound and moves with the geometry.
    constexpr uint32_t kFirstExcessive =
        AudioTimingGeometry::kTxSharedSlotPackets -
        2U * AudioTimingGeometry::kTxOwnershipGuardCycleSlots + 1U;
    for (uint32_t slack : {kFirstExcessive, UINT32_MAX,
                           UINT32_MAX - AudioTimingGeometry::kTxOwnershipGuardCycleSlots,
                           UINT32_MAX - 95U}) {
        auto request = Duet(); request.txDispatchSlackPackets = slack;
        EXPECT_EQ(ValidateTuning(request).rejection, TuningRejection::kPreparedTargetExceedsSharedSlots);
        auto state = Ready(); EXPECT_FALSE(state.Submit(request, depth));
    }
    EXPECT_TRUE(ValidateTuning(Duet()).Applicable());
}
TEST(AudioRuntimeTuningTransaction, PreparedHorizonHasTheSameDurationAtEveryRate) {
    // Bus cycles keep their duration as the sample rate changes, so the horizon
    // is a fixed number of microseconds and a rate-scaled number of frames.
    const uint32_t horizonUs =
        PacketsToMicroseconds(Duet().PreparedTargetPackets());
    for (uint32_t rate : {48000U, 96000U, 192000U})
        EXPECT_EQ(PreparedLeadFrames(Duet(), rate),
                  static_cast<uint32_t>(uint64_t{rate} * horizonUs / 1'000'000U));
}
TEST(AudioRuntimeTuningTransaction, TokensPreserveIdentityAndSeparateConfigurationNamespaces) {
    EXPECT_FALSE(IsTuningToken(1));
    for (uint32_t id : {1U, 123U, UINT32_MAX}) {
        EXPECT_TRUE(IsTuningToken(TuningToken(id)));
        EXPECT_EQ(static_cast<uint32_t>(TuningToken(id)), id);
    }
}
}
