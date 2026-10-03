// SPDX-License-Identifier: Apache-2.0
//
// The MOTU v3 servo ring lines fit one LogRecord.
//
// Before the split, [MotuSphServo] lost every field from `clamped=` on to the
// 232-byte record, so the flags, the mute and its counters were never seen on
// hardware. Each line is formatted here exactly as ASFW_LOG_RING_ONLY_RL does
// it, prefix included, with every field at its widest after a week of
// streaming at 48 kHz.

#include "Audio/DriverKit/MotuV3ServoTelemetryFormat.hpp"
#include "Logging/LogRing.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace {

constexpr size_t kMessageCapacity = sizeof(ASFW::Logging::LogRecord{}.message) - 1;

// A week of streaming: ~6200 bridge updates/s, one decision per ~10.7 ms,
// 48000 frames/s, ~25 M rx ticks/s. Each value is rounded up to all nines.
constexpr unsigned long long kGeneration = 4294967295ULL;
constexpr unsigned long long kUpdates = 9999999999ULL;
constexpr unsigned long long kDecisions = 99999999ULL;
constexpr unsigned long long kFrames = 99999999999ULL;
constexpr long long kRxTicks = 99999999999999LL;
constexpr long long kQ32 = -999999999999LL;     // |txCorrQ32| seen: 2.2e11
constexpr long long kStepQ32 = 9999999999999LL; // nominal 2199023255552
constexpr long long kPhaseTicks = -12288000LL;  // half the SPH tick domain
constexpr unsigned long long kEventCount = 999999ULL;

template <typename... Args>
std::string Format(const char* format, Args... args) {
    char buffer[512];
    const int length = std::snprintf(buffer, sizeof(buffer), format, args...);
    EXPECT_GT(length, 0);
    return std::string(buffer, static_cast<size_t>(length));
}

} // namespace

TEST(MotuV3ServoTelemetryFormatTests, ServoLineFitsOneRecordAtItsWidest) {
    const std::string message =
        Format("[DirectAudio][%s] " ASFW_MOTU_SPH_SERVO_FORMAT, ASFW_MOTU_SPH_SERVO_KEY,
               kGeneration, kUpdates, kDecisions, kFrames, kRxTicks, kQ32, kStepQ32, 1U,
               kPhaseTicks);
    EXPECT_LE(message.size(), kMessageCapacity) << message;
    // The last field is the one a cut would lose first.
    EXPECT_NE(message.rfind("phaseTicks=-12288000"), std::string::npos);
}

TEST(MotuV3ServoTelemetryFormatTests, StateLineFitsOneRecordAtItsWidest) {
    const std::string message =
        Format("[DirectAudio][%s] " ASFW_MOTU_SPH_STATE_FORMAT, ASFW_MOTU_SPH_STATE_KEY,
               kGeneration, kStepQ32, 1U, 1U, 1U, 1U, 1U, 1U, kEventCount, kEventCount,
               kEventCount, kEventCount, kFrames);
    EXPECT_LE(message.size(), kMessageCapacity) << message;
    EXPECT_NE(message.rfind("mutedFrames=99999999999"), std::string::npos);
}

// Offline analysers match this exact prefix; a field inserted before
// `locked=` would silently stop them matching.
TEST(MotuV3ServoTelemetryFormatTests, ServoLineKeepsTheAnalyzerPrefix) {
    const std::string message = Format(ASFW_MOTU_SPH_SERVO_FORMAT, 3ULL, 1ULL, 2ULL, 3ULL,
                                       4LL, 5LL, 6LL, 1U, 7LL);
    EXPECT_EQ(message,
              "[MotuSphServo] gen=3 updates=1 decisions=2 frames=3 rxTicks=4 txCorrQ32=5 "
              "stepQ32=6 locked=1 phaseTicks=7");
}
