// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AvcGraphBuilder.hpp"
#include "../Discovery/DiscoverySnapshot.hpp"
namespace ASFW::Protocols::AVC::Graph {
/// Pure graph projection; no transport or publication side effects.
[[nodiscard]] DeviceGraph BuildDiscoveryGraph(const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot,
                                              std::string modelName = {});
}
