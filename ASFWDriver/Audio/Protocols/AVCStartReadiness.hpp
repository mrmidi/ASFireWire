// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <cstdint>

namespace ASFW::Audio {

/// An AV/C stream start is valid only after discovery has rebound the stable
/// GUID to a real FireWire node and published that node's FCP transport.
/// Node values outside the 6-bit IEEE 1394 node space include ASFW's reset
/// sentinel (0xFFFF) and must not reach CMP/FCP setup.
[[nodiscard]] constexpr bool HasReadyAVCStartRoute(uint16_t nodeId,
                                                    bool hasFcpTransport) noexcept {
    return hasFcpTransport && nodeId < 64U;
}

} // namespace ASFW::Audio
