#pragma once

#include "LabAudioGeometry.hpp"

namespace ASFW::Lab {

// Keep the packet pump 1.5 nominal client buffers ahead of the largest ADK
// callback. This is transport preparation slack, independent of ZTS.
inline constexpr uint32_t kTransportPumpIntervalNs = 1'000'000;

[[nodiscard]] constexpr uint32_t TransportPreparationMarginFrames(
    const LabAudioGeometry& geometry) noexcept {
    return geometry.clientIoBudgetFrames + geometry.clientIoBudgetFrames / 2;
}

[[nodiscard]] constexpr uint32_t TransportCoverageFrames(
    const LabAudioGeometry& geometry) noexcept {
    return geometry.adkMaxClientIoFrames +
           TransportPreparationMarginFrames(geometry);
}

// At 48 kHz blocking mode, the cadence carries 8 frames in 3 of each 4
// cycles. Include two cycles for cadence phase at either boundary.
[[nodiscard]] constexpr uint32_t PacketSlotsForCoverageFrames(
    uint32_t frames) noexcept {
    const uint32_t dataPackets = (frames + 7) / 8;
    return (dataPackets * 4 + 2) / 3 + 2;
}

} // namespace ASFW::Lab
