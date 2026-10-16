// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeClockFit.hpp"
#include "RollingAffineFit.hpp"
namespace Pro40Audio {
// Diagnostic only: same epoch, continuity and consumed-prefix checks as the
// finite path, with recent observations retained instead of the whole session.
// 8192 data packets ~= 1.37 seconds; 128 controller pairs ~= 2 seconds at the
// existing staged cadence. Neither window size certifies clock quality.
// Absolute ordinals keep their meaning across windows and 8/128-second wraps.
// No HAL clock admission, latency calibration or nominal-rate substitution.
using SustainedPacketClockFit=BasicPacketClockFit<RollingAffineFit<8192>,691200000>;
using SustainedControllerClockFit=BasicControllerClockFit<RollingAffineFit<128>,691200000>;
using SustainedClockFit=BasicNativeClockFit<SustainedPacketClockFit,SustainedControllerClockFit>;
}
