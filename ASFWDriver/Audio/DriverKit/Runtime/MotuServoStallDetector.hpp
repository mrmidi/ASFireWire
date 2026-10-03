// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

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

// Off-hot-path edge detector. Reset once per receive activation and call from
// the watchdog queue. A healthy check is only comparisons of already-exported
// snapshots; this object performs no timing, formatting or logging.
class MotuServoStallDetector final {
  public:
    void Reset() noexcept { reported_ = false; }

    [[nodiscard]] bool Observe(
        uint64_t controlGeneration,
        const MotuRxSphCumulativeMeasurement& meter,
        const MotuRxSphClockSample& bridge,
        uint64_t bridgeUpdates,
        MotuServoStallEvent& out) noexcept {
        if (reported_ || controlGeneration == 0 || !meter.valid ||
            bridgeUpdates == 0 || bridge.streamGeneration != controlGeneration) {
            return false;
        }

        // Equality is healthy: the bridge commonly holds the meter's latest
        // accepted publication. A strict regression in either monotonic
        // dimension is the signature of a meter reset without a bridge reset.
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
