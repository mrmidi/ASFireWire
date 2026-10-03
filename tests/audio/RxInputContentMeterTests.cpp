#include <gtest/gtest.h>

#include "Audio/Engine/Direct/Rx/RxInputContentMeter.hpp"

#include <array>
#include <string>

using ASFW::AudioEngine::Direct::Rx::RxInputContentMeter;

namespace {

// Feeds `frames` copies of one frame; returns how many windows closed.
int Feed(RxInputContentMeter& meter, const float* frame, uint32_t channels,
         uint32_t frames) {
    int closed = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        closed += meter.ObserveFrame(frame, channels) ? 1 : 0;
    }
    return closed;
}

} // namespace

TEST(RxInputContentMeterTests, PeakDbRoundsDownToTheNextLowerStep) {
    EXPECT_EQ(RxInputContentMeter::PeakDb(1.0F), 0);
    EXPECT_EQ(RxInputContentMeter::PeakDb(1.5F), 0);
    EXPECT_EQ(RxInputContentMeter::PeakDb(0.5F), 7);    // -6.02 dBFS
    EXPECT_EQ(RxInputContentMeter::PeakDb(0.25F), 13);  // -12.04 dBFS
    EXPECT_EQ(RxInputContentMeter::PeakDb(0.1F), 20);   // -20.00 dBFS
    EXPECT_EQ(RxInputContentMeter::PeakDb(0.0F), -1);
    // Below half a 24-bit LSB the wire carries zero: silence, not signal.
    EXPECT_EQ(RxInputContentMeter::PeakDb(1.0F / 33554432.0F), -1);
    EXPECT_GE(RxInputContentMeter::PeakDb(1.0F / 8388608.0F), 138);
}

TEST(RxInputContentMeterTests, ReportsOncePerWindowWithPerChannelPeaks) {
    RxInputContentMeter meter;
    const std::array<float, 4> quiet{0.0F, 0.0F, 0.0F, 0.0F};
    const std::array<float, 4> loud{0.5F, -0.25F, 0.0F, 0.0F};

    EXPECT_EQ(Feed(meter, quiet.data(), 4, RxInputContentMeter::kWindowFrames - 1), 0);
    EXPECT_TRUE(meter.ObserveFrame(loud.data(), 4));
    EXPECT_EQ(meter.SignalMask(), 0x3U);
    EXPECT_EQ(std::string(meter.Line()), "ch=4 frames=96000 sig=0x00003 pk=-7,-13,-,-");

    // The next window starts clean: the old peaks do not leak into it.
    EXPECT_EQ(Feed(meter, quiet.data(), 4, RxInputContentMeter::kWindowFrames), 1);
    EXPECT_EQ(meter.SignalMask(), 0U);
    EXPECT_EQ(std::string(meter.Line()), "ch=4 frames=96000 sig=0x00000 pk=-,-,-,-");
}

TEST(RxInputContentMeterTests, ChannelCountChangeRestartsTheWindow) {
    RxInputContentMeter meter;
    const std::array<float, 18> frame{};
    EXPECT_EQ(Feed(meter, frame.data(), 18, RxInputContentMeter::kWindowFrames - 1), 0);
    // A different stream shape mid-window must not close the old window.
    EXPECT_FALSE(meter.ObserveFrame(frame.data(), 14));
    EXPECT_EQ(Feed(meter, frame.data(), 14, RxInputContentMeter::kWindowFrames - 2), 0);
    EXPECT_TRUE(meter.ObserveFrame(frame.data(), 14));
}

TEST(RxInputContentMeterTests, EighteenLoudChannelsFitTheRingRecord) {
    RxInputContentMeter meter;
    std::array<float, 18> frame{};
    frame.fill(1.0F / 8388608.0F); // the quietest signal: longest dB text
    EXPECT_EQ(Feed(meter, frame.data(), 18, RxInputContentMeter::kWindowFrames), 1);
    EXPECT_EQ(meter.SignalMask(), 0x3FFFFU);
    // LogRecord holds 231 characters; "[DirectAudio] [RxInputMeter] off=NN " is 36.
    EXPECT_LT(std::string(meter.Line()).size() + 36U, 231U);
}

TEST(RxInputContentMeterTests, NullOrEmptyFramesAreIgnored) {
    RxInputContentMeter meter;
    EXPECT_FALSE(meter.ObserveFrame(nullptr, 4));
    const float sample = 0.5F;
    EXPECT_FALSE(meter.ObserveFrame(&sample, 0));
}
