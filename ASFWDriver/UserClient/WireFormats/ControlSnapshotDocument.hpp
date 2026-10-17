// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ControlSnapshotDocument.hpp - one device's control topology and state as
// JSON for the app and MCP (FW-206). Served in the same paged envelope as the
// AV/C discovery document (SerializeDiscoveryPage), with `revision` in the
// page header's session field.
//
// Schema "asfw.controls.v1":
//   { "schema", "guid", "family", "revision",
//     "nodes":      [{ "id", "name", "kind": "endpoint|router|mixer|processor",
//                      "inputs": [portId], "outputs": [portId],
//                      "bundles": [{ "id", "routes": [[in, out]] }],   // routers
//                      "activeBundles": [bundleId],                    // routers
//                      "crosspoints": [{ "id", "in", "out" }] }],      // mixers
//     "ports":      [{ "id", "owner", "direction": "in|out", "channels", "name",
//                      "signalKind", "signalIndex" }],
//     "fixedLinks": [[sourcePort, destinationPort]],
//     "parameters": [{ "id", "name", "semantic", "target": {"node"|"port"|"crosspoint": id},
//                      "domain": { "kind": "bool" } | { "kind": "scalar", "min", "max",
//                                  "step"?, "unit" } | { "kind": "enum", "values": [{ "value", "name" }] },
//                      "value", "origin": "observed|cached|derived" }] }
#pragma once

#include "../../Audio/Model/Topology/State.hpp"
#include "../../Audio/Model/Topology/Topology.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace ASFW::UserClient::Wire {

inline constexpr std::string_view kControlSnapshotSchema = "asfw.controls.v1";

[[nodiscard]] std::string BuildControlSnapshotDocument(uint64_t guid, std::string_view family,
                                                       const AudioModel::Topology& topology,
                                                       const AudioModel::DeviceState& state, uint64_t sequence = 0);

} // namespace ASFW::UserClient::Wire
