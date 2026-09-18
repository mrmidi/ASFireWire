// SPDX-License-Identifier: Apache-2.0
//
// MotuLevelWriter tests: latest-value-wins coalescing with one write in flight and a
// cooldown between writes (documentation/AUDIO_BACKENDS_CONTROLS.md §5.3). The fake bus
// holds each write open until the test completes it, which is what an in-flight write is.

#include "Audio/Protocols/MOTU/MotuLevelWriter.hpp"
#include "FakeTimerScheduler.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ASFW::Audio::Motu::MotuLevelWriter;
using ASFW::Testing::FakeTimerScheduler;

struct HeldBus {
    struct Write {
        uint8_t value;
        MotuLevelWriter::DoneFn done;
    };
    std::vector<Write> writes;

    MotuLevelWriter::SendFn Send() {
        return [this](uint8_t value, MotuLevelWriter::DoneFn done) {
            writes.push_back(Write{value, std::move(done)});
        };
    }
    void Complete(size_t index, bool ok = true) { writes.at(index).done(ok); }
};

constexpr uint64_t kCooldown = MotuLevelWriter::kCooldownNs;

TEST(MotuLevelWriterTests, FirstValueGoesOutImmediately) {
    HeldBus bus;
    FakeTimerScheduler timers;
    MotuLevelWriter writer(bus.Send(), &timers);

    writer.Set(0x40);
    ASSERT_EQ(bus.writes.size(), 1U);
    EXPECT_EQ(bus.writes[0].value, 0x40);
}

TEST(MotuLevelWriterTests, ADragCoalescesToItsLatestValue) {
    HeldBus bus;
    FakeTimerScheduler timers;
    MotuLevelWriter writer(bus.Send(), &timers);

    writer.Set(0x40);
    for (uint8_t v = 0x41; v <= 0x60; ++v) {
        writer.Set(v); // arrives while 0x40 is still on the bus
    }
    ASSERT_EQ(bus.writes.size(), 1U);

    bus.Complete(0);
    EXPECT_EQ(bus.writes.size(), 1U) << "nothing may go out during the cooldown";

    timers.Advance(kCooldown);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[1].value, 0x60) << "only the final value of the burst is sent";
}

TEST(MotuLevelWriterTests, TheCooldownSpacesWritesEvenWithoutABacklog) {
    HeldBus bus;
    FakeTimerScheduler timers;
    MotuLevelWriter writer(bus.Send(), &timers);

    writer.Set(0x40);
    bus.Complete(0);
    writer.Set(0x50);
    EXPECT_EQ(bus.writes.size(), 1U);

    timers.Advance(kCooldown - 1);
    EXPECT_EQ(bus.writes.size(), 1U);
    timers.Advance(1);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[1].value, 0x50);
}

TEST(MotuLevelWriterTests, AValueTheDeviceAlreadyHasIsNotResent) {
    HeldBus bus;
    FakeTimerScheduler timers;
    MotuLevelWriter writer(bus.Send(), &timers);

    writer.Set(0x40);
    writer.Set(0x41);
    writer.Set(0x40); // back where the in-flight write is going
    bus.Complete(0);
    timers.Advance(kCooldown);
    EXPECT_EQ(bus.writes.size(), 1U);

    // After going idle, the next change goes out at once.
    writer.Set(0x42);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[1].value, 0x42);
}

TEST(MotuLevelWriterTests, AFailedWriteIsNotRetriedOnItsOwn) {
    HeldBus bus;
    FakeTimerScheduler timers;
    MotuLevelWriter writer(bus.Send(), &timers);

    writer.Set(0x40);
    bus.Complete(0, /*ok=*/false);
    timers.Advance(10 * kCooldown);
    EXPECT_EQ(bus.writes.size(), 1U) << "an unplugged device must not be written forever";

    writer.Set(0x40); // the host trying again is what retries
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[1].value, 0x40);
}

TEST(MotuLevelWriterTests, ANewerValueSurvivesTheFailureOfAnOlderOne) {
    HeldBus bus;
    FakeTimerScheduler timers;
    MotuLevelWriter writer(bus.Send(), &timers);

    writer.Set(0x40);
    writer.Set(0x50);
    bus.Complete(0, /*ok=*/false);
    timers.Advance(kCooldown);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[1].value, 0x50);
}

TEST(MotuLevelWriterTests, CancelStopsEverythingIncludingThePendingValue) {
    HeldBus bus;
    FakeTimerScheduler timers;
    {
        MotuLevelWriter writer(bus.Send(), &timers);
        writer.Set(0x40);
        writer.Set(0x50);
    } // destroyed with 0x40 still on the bus
    bus.Complete(0); // the late completion must be harmless
    timers.Advance(10 * kCooldown);
    EXPECT_EQ(bus.writes.size(), 1U);
}

TEST(MotuLevelWriterTests, WithoutASchedulerTheNextValueFollowsTheCompletion) {
    HeldBus bus;
    MotuLevelWriter writer(bus.Send(), nullptr);

    writer.Set(0x40);
    writer.Set(0x50);
    bus.Complete(0);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[1].value, 0x50);
}

} // namespace
