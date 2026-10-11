// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuServoStallDetector.hpp - edge detector for the MOTU RX SPH clock bridge.
//
// The bridge (motuRxSphClock) refuses any publication that does not move both
// cumulative counters forward. If the RX rate meter is reset while the bridge
// keeps the same start generation, the meter falls behind the bridge, every
// later publication is refused, and the transmit regulator silently stops
// receiving input. This detects exactly that: a meter strictly behind the
// bridge in the bridge's own generation.
//
// Off the hot path: call from the telemetry drain. It compares already
// exported snapshots and reports the first stall once per Reset(); it does no
// timing, formatting or logging itself.

#pragma once

#include "MotuRxSphClockSnapshot.hpp"
#include "MotuRxSphRateMeter.hpp"

#include <cstdint>

namespace ASFW::Audio::Runtime {

struct MotuServoStallEvent final {
    uint64_t streamGeneration{0};
    uint64_t bridgeUpdates{0};
    uint64_t bridgeFrames{0};
    int64_t bridgeTicks{0};
    uint64_t meterFrames{0};
    int64_t meterTicks{0};
};

class MotuServoStallDetector final {
  public:
    void Reset() noexcept { reported_ = false; }

    // True once, on the first observation of a meter behind the bridge.
    [[nodiscard]] bool Observe(uint64_t controlGeneration,
                               const MotuRxSphCumulativeMeasurement& meter,
                               const MotuRxSphClockSample& bridge,
                               uint64_t bridgeUpdates,
                               MotuServoStallEvent& out) noexcept {
        if (reported_ || controlGeneration == 0 || !meter.valid || bridgeUpdates == 0 ||
            bridge.streamGeneration != controlGeneration) {
            return false;
        }

        // Equality is healthy: the bridge usually holds the meter's latest
        // accepted publication.
        if (meter.frames >= bridge.rxFrames && meter.ticks >= bridge.rxTicks) {
            return false;
        }

        reported_ = true;
        out = {
            .streamGeneration = controlGeneration,
            .bridgeUpdates = bridgeUpdates,
            .bridgeFrames = bridge.rxFrames,
            .bridgeTicks = bridge.rxTicks,
            .meterFrames = meter.frames,
            .meterTicks = meter.ticks,
        };
        return true;
    }

  private:
    bool reported_{false};
};

} // namespace ASFW::Audio::Runtime
