// ToneCoreTests.cpp -- the tone glitch detector behind rtl_loopback --tone.
//
// Each case builds the capture the tool would record through a loopback: a
// stretch of silence (the round trip), then the tone with a little noise. It
// then injects one known defect and checks that the detector names it: a
// dropout (silence substituted in place), a slip (frames lost or repeated), a
// click. Rates are the ones the driver streams today.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

extern "C" {
#include "tone_core.h"
}

namespace {

constexpr double kFreq = 997.0;
constexpr float kAmp = 0.38f;          // the Pro 24 DSP loopback returned ~0.38
constexpr double kNoise = 2e-4;        // about the measured noise floor
constexpr uint32_t kLeadIn = 8835;     // a round trip's worth of silence
constexpr uint32_t kToneFrames = 3 * 48000;

// The tone as the output would carry it, phase-continuous from frame 0.
std::vector<float> Tone(double rate, uint32_t frames) {
    tone_gen_t g;
    tone_gen_init(&g, rate, kFreq, kAmp);
    std::vector<float> out(frames);
    for (auto& s : out) s = tone_gen_next(&g);
    return out;
}

// Deterministic noise so every run sees the same capture.
void AddNoise(std::vector<float>& x) {
    uint32_t state = 12345u;
    for (auto& s : x) {
        state = state * 1664525u + 1013904223u;
        const double u = (static_cast<double>(state >> 8) / 16777216.0) * 2.0 - 1.0;
        s += static_cast<float>(u * kNoise * std::sqrt(3.0));
    }
}

std::vector<float> Capture(const std::vector<float>& tone) {
    std::vector<float> x(kLeadIn, 0.0f);
    x.insert(x.end(), tone.begin(), tone.end());
    AddNoise(x);
    return x;
}

struct Analysis {
    tone_result_t result{};
    std::vector<tone_event_t> events;
};

Analysis Analyse(const std::vector<float>& x, double rate) {
    Analysis a;
    a.events.resize(64);
    EXPECT_EQ(tone_analyse(x.data(), x.size(), rate, kFreq, a.events.data(),
                           static_cast<uint32_t>(a.events.size()), &a.result),
              0);
    a.events.resize(a.result.storedEvents);
    return a;
}

class ToneCore : public ::testing::TestWithParam<double> {};

TEST_P(ToneCore, CleanToneHasNoEvents) {
    const double rate = GetParam();
    const auto a = Analyse(Capture(Tone(rate, kToneFrames)), rate);
    ASSERT_TRUE(a.result.valid) << a.result.invalidReason;
    EXPECT_EQ(a.result.eventCount, 0u);
    EXPECT_NEAR(a.result.amplitude, kAmp, kAmp * 0.02);
    EXPECT_LT(a.result.noiseRms, 5 * kNoise);
    // The tone starts after the lead-in, to within one fitted block.
    EXPECT_NEAR(static_cast<double>(a.result.onsetSample), kLeadIn, TONE_BLOCK);
}

TEST_P(ToneCore, OnePacketOfSilenceIsADropout) {
    const double rate = GetParam();
    auto tone = Tone(rate, kToneFrames);
    const uint32_t at = 50000;
    for (uint32_t i = 0; i < 8; ++i) tone[at + i] = 0.0f;  // one 8-frame packet
    const auto a = Analyse(Capture(tone), rate);
    ASSERT_TRUE(a.result.valid) << a.result.invalidReason;
    ASSERT_EQ(a.result.eventCount, 1u);
    const auto& e = a.events[0];
    EXPECT_EQ(e.kind, TONE_EVENT_DROPOUT);
    EXPECT_NEAR(static_cast<double>(e.firstSample), kLeadIn + at, 2);
    EXPECT_NEAR(static_cast<double>(e.lastSample), kLeadIn + at + 7, 2);
    EXPECT_GE(e.silentSamples, 6u);
    EXPECT_LT(std::fabs(e.slipFrames), 0.25);
}

TEST_P(ToneCore, LongSilenceIsOneDropout) {
    const double rate = GetParam();
    auto tone = Tone(rate, kToneFrames);
    const uint32_t at = 60000;
    for (uint32_t i = 0; i < 1000; ++i) tone[at + i] = 0.0f;
    const auto a = Analyse(Capture(tone), rate);
    ASSERT_TRUE(a.result.valid) << a.result.invalidReason;
    ASSERT_EQ(a.result.eventCount, 1u);
    EXPECT_EQ(a.events[0].kind, TONE_EVENT_DROPOUT);
    EXPECT_GE(a.events[0].silentSamples, 900u);
}

TEST_P(ToneCore, LostFramesAreAPositiveSlip) {
    const double rate = GetParam();
    const auto full = Tone(rate, kToneFrames + 5);
    const uint32_t at = 70000;
    std::vector<float> tone(full.begin(), full.begin() + at);
    tone.insert(tone.end(), full.begin() + at + 5, full.end());  // 5 frames never arrive
    const auto a = Analyse(Capture(tone), rate);
    ASSERT_TRUE(a.result.valid) << a.result.invalidReason;
    ASSERT_EQ(a.result.eventCount, 1u);
    EXPECT_EQ(a.events[0].kind, TONE_EVENT_SLIP);
    EXPECT_NEAR(a.events[0].slipFrames, 5.0, 0.1);
    EXPECT_NEAR(static_cast<double>(a.events[0].firstSample), kLeadIn + at, TONE_BLOCK);
}

TEST_P(ToneCore, RepeatedFramesAreANegativeSlip) {
    const double rate = GetParam();
    const auto full = Tone(rate, kToneFrames);
    const uint32_t at = 80000;
    std::vector<float> tone(full.begin(), full.begin() + at);
    tone.insert(tone.end(), full.begin() + at - 3, full.begin() + at);  // 3 frames twice
    tone.insert(tone.end(), full.begin() + at, full.end());
    const auto a = Analyse(Capture(tone), rate);
    ASSERT_TRUE(a.result.valid) << a.result.invalidReason;
    ASSERT_EQ(a.result.eventCount, 1u);
    EXPECT_EQ(a.events[0].kind, TONE_EVENT_SLIP);
    EXPECT_NEAR(a.events[0].slipFrames, -3.0, 0.1);
}

TEST_P(ToneCore, ASpikeIsAClick) {
    const double rate = GetParam();
    auto tone = Tone(rate, kToneFrames);
    const uint32_t at = 90000;
    tone[at] += 0.2f;
    const auto a = Analyse(Capture(tone), rate);
    ASSERT_TRUE(a.result.valid) << a.result.invalidReason;
    ASSERT_EQ(a.result.eventCount, 1u);
    EXPECT_EQ(a.events[0].kind, TONE_EVENT_CLICK);
    EXPECT_EQ(a.events[0].firstSample, kLeadIn + at);
    EXPECT_EQ(a.events[0].lastSample, kLeadIn + at);
    EXPECT_NEAR(a.events[0].peakResidual, 0.2 / kAmp, 0.05);
}

// The hardware shape seen on 2026-09-26: the phase is knocked away and comes
// back a few hundred frames later. That is one disturbance with a net slip of
// zero, not a string of clicks.
TEST_P(ToneCore, ADisturbanceThatRecoversIsOneEvent) {
    const double rate = GetParam();
    const auto full = Tone(rate, kToneFrames);
    const uint32_t at = 60000;
    std::vector<float> tone(full.begin(), full.begin() + at);
    tone.insert(tone.end(), full.begin() + at - 16, full.begin() + at);  // 16 repeated
    tone.insert(tone.end(), full.begin() + at, full.begin() + at + 400);
    tone.insert(tone.end(), full.begin() + at + 416, full.end());        // 16 skipped
    const auto a = Analyse(Capture(tone), rate);
    ASSERT_TRUE(a.result.valid) << a.result.invalidReason;
    ASSERT_EQ(a.result.eventCount, 1u);
    EXPECT_EQ(a.events[0].kind, TONE_EVENT_CLICK);
    EXPECT_NEAR(a.events[0].slipFrames, 0.0, 0.1);
    EXPECT_GE(a.events[0].lastSample - a.events[0].firstSample, 350u);
}

TEST_P(ToneCore, SeparateDefectsAreSeparateEvents) {
    const double rate = GetParam();
    auto tone = Tone(rate, kToneFrames);
    for (uint32_t i = 0; i < 8; ++i) tone[40000 + i] = 0.0f;
    tone[100000] -= 0.3f;
    const auto a = Analyse(Capture(tone), rate);
    ASSERT_TRUE(a.result.valid) << a.result.invalidReason;
    ASSERT_EQ(a.result.eventCount, 2u);
    EXPECT_EQ(a.result.counts[TONE_EVENT_DROPOUT], 1u);
    EXPECT_EQ(a.result.counts[TONE_EVENT_CLICK], 1u);
    EXPECT_LT(a.events[0].firstSample, a.events[1].firstSample);
}

INSTANTIATE_TEST_SUITE_P(Rates, ToneCore, ::testing::Values(44100.0, 48000.0),
                         [](const auto& info) {
                             return info.param == 44100.0 ? std::string("Rate44k1")
                                                          : std::string("Rate48k");
                         });

TEST(ToneCoreInvalid, SilenceIsNotAMeasurement) {
    std::vector<float> x(48000, 0.0f);
    AddNoise(x);
    tone_result_t r{};
    ASSERT_EQ(tone_analyse(x.data(), x.size(), 48000.0, kFreq, nullptr, 0, &r), 0);
    EXPECT_FALSE(r.valid);
    ASSERT_NE(r.invalidReason, nullptr);
}

TEST(ToneCoreInvalid, TooShortIsNotAMeasurement) {
    std::vector<float> x(TONE_BLOCK * 8, 0.1f);
    tone_result_t r{};
    ASSERT_EQ(tone_analyse(x.data(), x.size(), 48000.0, kFreq, nullptr, 0, &r), 0);
    EXPECT_FALSE(r.valid);
}

}  // namespace
