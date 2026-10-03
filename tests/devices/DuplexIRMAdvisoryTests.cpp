#include <gtest/gtest.h>

#include "Audio/Protocols/Backends/DuplexIRMAdvisory.hpp"

namespace {

using ASFW::Audio::AudioStreamWireInfo;
using ASFW::Audio::Backends::AdvisoryFallbackChannel;
using ASFW::Audio::Backends::SoleChannelInMask;

constexpr uint8_t kNoFallback = AudioStreamWireInfo::kInvalidIsoChannel;
constexpr uint64_t kMotuPlaybackMask = uint64_t{1} << 0U;
constexpr uint64_t kMotuCaptureMask = uint64_t{1} << 1U;
constexpr uint64_t kAllChannels = ~uint64_t{0};

// The blocker this policy exists for: a bare two-node MacBook<->MOTU bus has no
// IRM node at all, so IRMClient reports NotFound -> kIOReturnNoDevice.
TEST(DuplexIRMAdvisoryTests, AbsentIrmFallsBackToDeviceAssignedChannel) {
    EXPECT_EQ(AdvisoryFallbackChannel(kIOReturnNoDevice, kMotuPlaybackMask,
                                      /*deviceOwnsChannel=*/true),
              0U);
    EXPECT_EQ(AdvisoryFallbackChannel(kIOReturnNoDevice, kMotuCaptureMask,
                                      /*deviceOwnsChannel=*/true),
              1U);
}

// A designated IRM that never answers is the same situation as none at all.
TEST(DuplexIRMAdvisoryTests, UnresponsiveIrmAlsoFallsBack) {
    EXPECT_EQ(AdvisoryFallbackChannel(kIOReturnTimeout, kMotuPlaybackMask,
                                      /*deviceOwnsChannel=*/true),
              0U);
}

// The safety property that keeps this from being a silent regression: a real
// conflict means another node holds the channel or the bandwidth, and starting
// anyway would transmit on top of it. Likewise a bus reset invalidates the epoch.
TEST(DuplexIRMAdvisoryTests, GenuineResourceConflictStaysFatal) {
    EXPECT_EQ(AdvisoryFallbackChannel(kIOReturnNoResources, kMotuPlaybackMask,
                                      /*deviceOwnsChannel=*/true),
              kNoFallback);
    EXPECT_EQ(AdvisoryFallbackChannel(kIOReturnOffline, kMotuPlaybackMask,
                                      /*deviceOwnsChannel=*/true),
              kNoFallback);
    EXPECT_EQ(AdvisoryFallbackChannel(kIOReturnError, kMotuPlaybackMask,
                                      /*deviceOwnsChannel=*/true),
              kNoFallback);
}

// CMP/BeBoB devices depend on the IRM to *select* a channel, so there is no
// device-assigned answer to fall back to and the failure must propagate.
TEST(DuplexIRMAdvisoryTests, CmpDeviceNeverDegrades) {
    EXPECT_EQ(AdvisoryFallbackChannel(kIOReturnNoDevice, kAllChannels,
                                      /*deviceOwnsChannel=*/false),
              kNoFallback);
    // Even a mis-set flag cannot rescue a mask that names more than one channel.
    EXPECT_EQ(AdvisoryFallbackChannel(kIOReturnNoDevice, kAllChannels,
                                      /*deviceOwnsChannel=*/true),
              kNoFallback);
}

TEST(DuplexIRMAdvisoryTests, AmbiguousOrEmptyMaskFailsClosed) {
    EXPECT_EQ(SoleChannelInMask(0), kNoFallback);
    EXPECT_EQ(SoleChannelInMask(0b0110), kNoFallback);
    EXPECT_EQ(SoleChannelInMask(uint64_t{1} << 63U), 63U);
}

} // namespace
