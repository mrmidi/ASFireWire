#include "Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <gtest/gtest.h>

#include <array>

namespace {

TEST(AmdtpRateGeometryTests, StandardRatesKeepNominalAndSytIntervalDistinct) {
    struct Expected {
        uint32_t rate;
        uint32_t nominal;
        uint32_t interval;
    };
    constexpr std::array expected{
        Expected{32000, 4, 8},
        Expected{44100, 6, 8},
        Expected{48000, 6, 8},
        Expected{88200, 12, 16},
        Expected{96000, 12, 16},
        Expected{176400, 24, 32},
        Expected{192000, 24, 32},
    };

    for (const auto& value : expected) {
        const auto geometry = ASFW::Encoding::AmdtpRateGeometryForSampleRate(value.rate);
        ASSERT_TRUE(geometry.has_value());
        EXPECT_EQ(geometry->sampleRateHz, value.rate);
        EXPECT_EQ(geometry->nominalFramesPerCycle, value.nominal);
        EXPECT_EQ(geometry->sytIntervalFrames, value.interval);
    }
    EXPECT_FALSE(ASFW::Encoding::AmdtpRateGeometryForSampleRate(96001).has_value());
}

} // namespace
