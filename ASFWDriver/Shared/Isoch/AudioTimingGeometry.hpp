#pragma once

#include <cstdint>

namespace ASFW::IsochTransport {

struct AudioTimingGeometry final {
    static constexpr uint32_t kSampleRateHz = 48000;
    static constexpr uint32_t kFrameRingFrames = 512;
    static constexpr uint32_t kHalIoPeriodFrames = kFrameRingFrames;
    static constexpr uint32_t kHalZeroTimestampPeriodFrames = kFrameRingFrames;
    static constexpr uint32_t kFrameAlignment = 32;
    static constexpr uint32_t kFramesPerDataPacket = 8;
    static constexpr uint32_t kRxPacketsPerGroup = 8;
    static constexpr uint32_t kTxPacketsPerGroup = 8;
    static constexpr uint32_t kTimingGroupPackets =
        kRxPacketsPerGroup;
    static constexpr uint32_t kTxHardwareRingPackets = 192;
    static constexpr uint32_t kTxPreparationLeadPackets =
        kTxHardwareRingPackets + kTxPacketsPerGroup;
};

static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kHalIoPeriodFrames ==
              0,
              "Frame ring must be an integer number of HAL IO periods");
static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kHalZeroTimestampPeriodFrames ==
              0,
              "Frame ring must be an integer number of ZTS periods");
static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kFrameAlignment ==
              0,
              "Frame ring must satisfy the 32-frame alignment contract");
static_assert(AudioTimingGeometry::kTimingGroupPackets != 0,
              "Timing group packet count must be non-zero");
static_assert(AudioTimingGeometry::kRxPacketsPerGroup ==
                  AudioTimingGeometry::kTxPacketsPerGroup,
              "RX and TX interrupt groups must match");

} // namespace ASFW::IsochTransport
