// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcExtensionInventory.hpp - The read-only vendor inventories that follow
// generic AV/C discovery, at attach and on every refresh.
//
// BridgeCo: the unit plug format lists, channel positions, sections and signal
// formats (Linux bebob_stream.c:908-940); the formations also size graph
// streams the descriptors could not (Phase 88). Oxford: the stream-format list
// in both directions (Linux oxfw-stream.c:552-622). Each keeps the unit alive
// until it finishes and calls `done` exactly once.

#pragma once

#include "AVCUnit.hpp"
#include "AvcProbeAdmission.hpp"

namespace ASFW::Protocols::AVC {

[[nodiscard]] AVCUnit::DiscoveryOptions DiscoveryOptionsFor(AvcExtensionInventory inventory);

} // namespace ASFW::Protocols::AVC
