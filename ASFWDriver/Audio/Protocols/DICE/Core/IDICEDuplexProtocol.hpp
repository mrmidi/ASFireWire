// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IDICEDuplexProtocol.hpp - Transitional DICE compatibility alias

#pragma once

#include "../../Duplex/IDuplexDeviceControl.hpp"

namespace ASFW::Audio::DICE {

// DICE implementations are the first IDuplexDeviceControl providers. Keep this
// alias only until FW-73 moves/removes the legacy DICE-named header.
using IDICEDuplexProtocol = ::ASFW::Audio::IDuplexDeviceControl;

} // namespace ASFW::Audio::DICE
