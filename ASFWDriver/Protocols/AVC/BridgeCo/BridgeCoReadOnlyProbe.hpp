// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Bounded, observational BeBoB discovery for exact known devices.

#pragma once

#include "../IAVCCommandSubmitter.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::Protocols::AVC::BridgeCo {

inline constexpr uint32_t kTerraTecVendorId = 0x000aac;
inline constexpr uint32_t kPhase88RackFwModelId = 0x000003;

[[nodiscard]] constexpr bool IsTerraTecPhase88RackFw(uint32_t vendorId,
                                                       uint32_t modelId) noexcept {
    return vendorId == kTerraTecVendorId && modelId == kPhase88RackFwModelId;
}

struct StreamFormation {
    uint8_t rateCode{0};
    uint8_t pcmChannels{0};
    uint8_t midiSlots{0};
};

// A BridgeCo extended stream-format-list response echoes the requested list
// index in operand 7; the AM824 formation begins at operand 8.  Keeping this
// codec public makes the discovery trace and the BeBoB backend share exactly
// one wire interpretation.
[[nodiscard]] std::optional<StreamFormation>
ParseExtendedStreamFormatListResponse(uint8_t requestedIndex,
                                      std::span<const uint8_t> operands) noexcept;

[[nodiscard]] std::optional<StreamFormation>
ParseStreamFormation(std::span<const uint8_t> formation) noexcept;

/// Sends STATUS queries only. It never changes device clock, rate, routing,
/// CMP, PCR, or stream state.
void StartPhase88ReadOnlyProbe(IAVCCommandSubmitter& submitter, uint64_t guid);

} // namespace ASFW::Protocols::AVC::BridgeCo
