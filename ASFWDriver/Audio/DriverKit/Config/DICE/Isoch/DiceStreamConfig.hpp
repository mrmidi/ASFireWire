// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceStreamConfig.hpp
// Isoch stream configuration parameters.

#pragma once

#include "../../AudioStreamProfile.hpp"

namespace ASFW::Isoch::Audio::DICE {

// Compatibility aliases for DICE-specific profiles. The packet geometry
// contract itself is protocol-neutral; AV/C/BeBoB uses the same ADK seam.
using DiceStreamDirection = AudioStreamDirection;
using DiceStreamConfig = AudioStreamConfig;

} // namespace ASFW::Isoch::Audio::DICE
