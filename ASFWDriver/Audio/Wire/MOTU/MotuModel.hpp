// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once
#include "MotuBlockLayout.hpp"
#include <array>
#include <cstdint>

namespace ASFW::Encoding::Motu {
enum class ProtocolVersion : uint8_t { V1 = 1, V2, V3 };
enum class OpticalLayout : uint8_t { None, V2, V2EightPre, V3Banks };
enum class FetchRule : uint8_t { None, Traveler, Spartan, V3 };
struct Model final {
    uint32_t unitVersion;
    const char* name;
    ProtocolVersion protocol;
    std::array<uint32_t, 3> captureChunks;
    std::array<uint32_t, 3> playbackChunks;
    OpticalLayout optical;
    FetchRule fetch;
    // Conflicting vendor/Linux geometry is recorded, not silently selected.
    uint8_t unresolvedModes{0};
};
// Wire facts: references/linux-sound-firewire-stack/firewire/motu/
// motu.c:164-180; motu-protocol-v2.c:274-320; motu-protocol-v3.c:269-359.
// Vendor disagreements: tmp/motu-research/vendor-four-gaps.md. Counts include
// padding chunks; they must never be interpreted as physical port counts.
inline constexpr std::array kModels{
    Model{0x1, "828", ProtocolVersion::V1, {}, {}, OpticalLayout::None, FetchRule::None, 7},
    Model{0x2, "896", ProtocolVersion::V1, {}, {}, OpticalLayout::None, FetchRule::None, 7},
    Model{0x3, "828mk2", ProtocolVersion::V2, {14,14,0}, {14,14,0}, OpticalLayout::V2, FetchRule::None},
    Model{0x5, "896HD", ProtocolVersion::V2, {14,14,8}, {14,14,8}, OpticalLayout::V2, FetchRule::None},
    Model{0x9, "Traveler", ProtocolVersion::V2, {14,14,8}, {14,14,8}, OpticalLayout::V2, FetchRule::Traveler},
    Model{0xd, "UltraLite", ProtocolVersion::V2, {14,14,0}, {14,14,0}, OpticalLayout::None, FetchRule::Spartan},
    Model{0xf, "8pre", ProtocolVersion::V2, {10,10,0}, {6,6,0}, OpticalLayout::V2EightPre, FetchRule::Spartan},
    Model{0x15, "828mk3", ProtocolVersion::V3, {18,18,14}, {14,14,10}, OpticalLayout::V3Banks, FetchRule::V3},
    Model{0x17, "896mk3", ProtocolVersion::V3, {18,14,10}, {18,14,10}, OpticalLayout::V3Banks, FetchRule::V3, 1},
    Model{0x19, "UltraLite mk3", ProtocolVersion::V3, {18,14,10}, {14,14,14}, OpticalLayout::None, FetchRule::V3},
    Model{0x1b, "Traveler mk3", ProtocolVersion::V3, {18,14,10}, {14,14,10}, OpticalLayout::V3Banks, FetchRule::V3},
    Model{0x30, "UltraLite mk3 Hybrid", ProtocolVersion::V3, {18,14,10}, {14,14,14}, OpticalLayout::None, FetchRule::V3},
    Model{0x33, "Audio Express", ProtocolVersion::V3, {10,10,0}, {10,10,0}, OpticalLayout::None, FetchRule::V3},
    Model{0x35, "828mk3 Hybrid", ProtocolVersion::V3, {18,18,14}, {14,14,14}, OpticalLayout::V3Banks, FetchRule::V3, 4},
    Model{0x37, "896mk3 Hybrid", ProtocolVersion::V3, {18,14,10}, {18,14,10}, OpticalLayout::V3Banks, FetchRule::V3, 5},
    Model{0x39, "Track16", ProtocolVersion::V3, {14,14,14}, {6,6,6}, OpticalLayout::V3Banks, FetchRule::V3, 7},
    Model{0x45, "4pre", ProtocolVersion::V3, {10,10,0}, {10,10,0}, OpticalLayout::None, FetchRule::V3},
};
[[nodiscard]] constexpr const Model* FindModel(uint32_t version) noexcept {
    for (const auto& model : kModels) if (model.unitVersion == version) return &model;
    return nullptr;
}
[[nodiscard]] constexpr bool SupportsRate(const Model& model, uint32_t rate) noexcept {
    const auto index = RateToIndex(rate);
    if (index < 0) return false;
    const auto mode = IndexToMode(static_cast<uint32_t>(index));
    return !(model.unresolvedModes & (1U << mode)) &&
        model.captureChunks[mode] && model.playbackChunks[mode];
}
} // namespace ASFW::Encoding::Motu
