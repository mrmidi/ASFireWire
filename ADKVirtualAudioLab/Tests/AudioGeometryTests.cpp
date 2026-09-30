#include "TestHarness.hpp"
#include "../Core/LabAudioGeometry.hpp"
#include "../Core/TransportPumpGeometry.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpPacketTimeline.hpp"

namespace ASFW::LabTests {

void RunAudioGeometryTests(TestContext& ctx) {
    constexpr auto geometry = ASFW::Lab::AudioGeometryForRate(48'000);
    CHECK(ctx, ASFW::Lab::IsValidAudioGeometry(geometry));
    CHECK_EQ_U32(ctx, geometry.sampleRateHz, 48'000);
    CHECK_EQ_U32(ctx, geometry.activeRingFrames, 12'288);
    CHECK_EQ_U32(ctx, geometry.allocatedRingFrames, 24'576);
    CHECK_EQ_U32(ctx, geometry.zeroTimestampPeriodFrames, 12'288);
    CHECK_EQ_U32(ctx, geometry.clientIoBudgetFrames, 1'024);
    CHECK_EQ_U32(ctx, geometry.adkMaxClientIoFrames, 4'096);

    // The second half of the descriptor is allocated capacity, not active
    // HAL ring space. PCM addressing always wraps at activeRingFrames.
    CHECK_EQ_U32(ctx, ASFW::Lab::ActiveRingFrame(12'287, geometry), 12'287);
    CHECK_EQ_U32(ctx, ASFW::Lab::ActiveRingFrame(12'288, geometry), 0);
    CHECK_EQ_U32(ctx, ASFW::Lab::ActiveRingFrame(24'575, geometry), 12'287);
    CHECK_EQ_U32(ctx, ASFW::Lab::ActiveRingFrame(24'576, geometry), 0);

    constexpr uint32_t coverage = ASFW::Lab::TransportCoverageFrames(geometry);
    CHECK_EQ_U32(ctx, coverage, 5'632);
    CHECK(ctx, ASFW::Lab::PacketSlotsForCoverageFrames(coverage) <=
                   ASFW::Protocols::Audio::AMDTP::kAmdtpPacketHistorySlots);
}

} // namespace ASFW::LabTests
