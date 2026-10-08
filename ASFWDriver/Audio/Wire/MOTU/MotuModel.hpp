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
enum class TimingPolicy : uint8_t { ReplayObserved, SynthesizedExperimental };
#ifndef ASFW_MOTU_SYNTH_VALIDATION
#define ASFW_MOTU_SYNTH_VALIDATION 0
#endif
inline constexpr bool kSynthHardwareBatch = ASFW_MOTU_SYNTH_VALIDATION != 0;
// Synthesis remains a model opt-in experiment until equivalent behavior is
// measured. Default builds preserve the observed-offset replay seam.
inline constexpr TimingPolicy kExperimentalTiming = kSynthHardwareBatch ?
    TimingPolicy::SynthesizedExperimental : TimingPolicy::ReplayObserved;
struct RuntimePolicy final {
    TimingPolicy timing{TimingPolicy::ReplayObserved};
    bool requireTimingBeforeFetch{false};
    uint32_t minimumHostRunMs{0};
    bool deactivateBeforeStart{false};
    bool clearIsoCommLowBits{false};
    bool write48kStreamConfig{false};
};
inline constexpr RuntimePolicy kReplayFetch{.requireTimingBeforeFetch = true};
inline constexpr RuntimePolicy kOriginal828{.requireTimingBeforeFetch = true, .minimumHostRunMs = 100};
inline constexpr RuntimePolicy kSynthFetch{.timing = kExperimentalTiming, .requireTimingBeforeFetch = true};
inline constexpr RuntimePolicy k828mk3Policy{.timing = kExperimentalTiming,
    .requireTimingBeforeFetch = true, .deactivateBeforeStart = true,
    .clearIsoCommLowBits = true, .write48kStreamConfig = true};
#ifndef ASFW_MOTU_V1_VALIDATION
#define ASFW_MOTU_V1_VALIDATION 0
#endif
inline constexpr bool kV1HardwareBatch = ASFW_MOTU_V1_VALIDATION != 0;
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
    RuntimePolicy runtime{};
};
// Wire facts: references/linux-sound-firewire-stack/firewire/motu/
// motu.c:164-180; motu-protocol-v2.c:274-320; motu-protocol-v3.c:269-359.
// Vendor disagreements: tmp/motu-research/vendor-four-gaps.md. Counts include
// padding chunks; they must never be interpreted as physical port counts.
inline constexpr std::array kModels{
    Model{0x1, "828", ProtocolVersion::V1, {10,0,0}, {10,0,0}, OpticalLayout::None, FetchRule::None, 0, kOriginal828},
    Model{0x2, "896", ProtocolVersion::V1, {10,10,0}, {10,10,0}, OpticalLayout::None, FetchRule::None, 2, kReplayFetch},
    Model{0x3, "828mk2", ProtocolVersion::V2, {14,14,0}, {14,14,0}, OpticalLayout::V2, FetchRule::None, 0, kReplayFetch},
    Model{0x5, "896HD", ProtocolVersion::V2, {14,14,8}, {14,14,8}, OpticalLayout::V2, FetchRule::None, 0, kReplayFetch},
    Model{0x9, "Traveler", ProtocolVersion::V2, {14,14,8}, {14,14,8}, OpticalLayout::V2, FetchRule::Traveler, 0, kReplayFetch},
    Model{0xd, "UltraLite", ProtocolVersion::V2, {14,14,0}, {14,14,0}, OpticalLayout::None, FetchRule::Spartan, 0, kReplayFetch},
    Model{0xf, "8pre", ProtocolVersion::V2, {10,10,0}, {6,6,0}, OpticalLayout::V2EightPre, FetchRule::Spartan, 0, kReplayFetch},
    Model{0x15, "828mk3", ProtocolVersion::V3, {18,18,14}, {14,14,10}, OpticalLayout::V3Banks, FetchRule::V3, 0, k828mk3Policy},
    Model{0x17, "896mk3", ProtocolVersion::V3, {18,14,10}, {18,14,10}, OpticalLayout::V3Banks, FetchRule::V3, 0, kSynthFetch},
    Model{0x19, "UltraLite mk3", ProtocolVersion::V3, {18,14,10}, {14,14,14}, OpticalLayout::None, FetchRule::V3, 0, kSynthFetch},
    Model{0x1b, "Traveler mk3", ProtocolVersion::V3, {18,14,10}, {14,14,10}, OpticalLayout::V3Banks, FetchRule::V3, 0, kSynthFetch},
    Model{0x30, "UltraLite mk3 Hybrid", ProtocolVersion::V3, {18,14,10}, {14,14,14}, OpticalLayout::None, FetchRule::V3},
    Model{0x33, "Audio Express", ProtocolVersion::V3, {10,10,0}, {10,10,0}, OpticalLayout::None, FetchRule::V3},
    Model{0x35, "828mk3 Hybrid", ProtocolVersion::V3, {18,18,14}, {14,14,14}, OpticalLayout::V3Banks, FetchRule::V3, 4},
    Model{0x37, "896mk3 Hybrid", ProtocolVersion::V3, {18,14,10}, {18,14,10}, OpticalLayout::V3Banks, FetchRule::V3, 5},
    Model{0x39, "Track16", ProtocolVersion::V3, {14,14,14}, {6,6,6}, OpticalLayout::V3Banks, FetchRule::V3, 7},
    Model{0x45, "4pre", ProtocolVersion::V3, {10,10,0}, {10,10,0}, OpticalLayout::None, FetchRule::V3},
};
// Vendor 896mk3 SetupStrmFwIds @0x3f118 compares provider Gestalt to
// 0x31333934 ("1394"): FireWire selects 18 playback chunks at 1x, matching
// Linux motu-protocol-v3.c. The 16-chunk branch belongs to other providers.
[[nodiscard]] constexpr bool FireWireOnly(uint32_t version) noexcept {
    switch (version) {
    case 1: case 2: case 3: case 5: case 9: case 0xd: case 0xf:
    case 0x15: case 0x17: case 0x19: case 0x1b: return true;
    default: return false;
    }
}
// Linux motu-protocol-v1.c:394-461: V1 PCM starts immediately after SPH.
// Original 828 capture has two trailing status chunks; playback has none.
[[nodiscard]] constexpr uint32_t MessageChunks(uint32_t version, bool capture) noexcept {
    return version == 1 ? (capture ? 2U : 0U) : version == 2 ? 0U : 2U;
}
[[nodiscard]] constexpr uint32_t PcmByteOffset(uint32_t version) noexcept {
    return version == 1 || version == 2 ? 4U : 10U;
}
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
