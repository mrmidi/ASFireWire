// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiscoveryLog.hpp - What one AV/C unit's discovery found, as named ring lines.
//
// A discovery result used to reach the ring only as "result=completed". This turns a committed
// DiscoverySnapshot (and the stream graph built from it) into one line per discovered fact, every wire
// value shown as its spec name plus its raw value (`ready(0x3)`), every value with no name shown as
// `UNKNOWN(<table>:0xNN)`, and every departure from the spec shown as a deviation. A log reader sees
// what the device said, in the spec's words, without decoding bytes.
//
// DescribeDiscovery is pure: the same snapshot gives the same lines, so a test pins every name against
// a golden file. The single caller that writes the lines to the ring is AVCUnit.
//
// A ring message holds 232 bytes, so a line is a single fact and a long one is wrapped into labelled
// continuation lines ("(cont N)"); nothing is truncated.

#pragma once

#include "DiscoverySnapshot.hpp"
#include "../Graph/AvcDeviceGraph.hpp"

#include <string>
#include <vector>

namespace ASFW::AVC::DiscoveryEngine {

/// Longest line DescribeDiscovery returns (below the ring's 232-byte message, with room for the prefix).
inline constexpr size_t kMaxDiscoveryLogLine = 200;

/// Every line starts with this, so the ring can be filtered with a `contains` query.
inline constexpr const char* kDiscoveryLogTag = "[AvcCaps]";

/// One line per discovered fact. `graph` may be null (an incomplete discovery has none).
[[nodiscard]] std::vector<std::string> DescribeDiscovery(const DiscoverySnapshot& snapshot,
                                                         const Protocols::AVC::Graph::DeviceGraph* graph);

} // namespace ASFW::AVC::DiscoveryEngine
