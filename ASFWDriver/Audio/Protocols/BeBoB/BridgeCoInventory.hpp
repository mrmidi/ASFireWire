// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BridgeCoInventory.hpp - What a BridgeCo (BeBoB) unit adds to generic AV/C
// discovery.
//
// Generic discovery already reads the unit plug counts, each unit plug's
// stream-format list (0x2F, as BridgeCo requires) and its signal format. The
// only BridgeCo-specific facts are the channel positions and section types of
// ISO plug 0 in each direction (EXTENDED PLUG INFO, Linux
// sound/firewire/bebob/bebob_command.c:91-107, 289-328), which give the AM824
// slot order (bebob_stream.c:254-372). This inventory sends exactly those
// STATUS queries and derives everything else from the snapshot.
//
// Fresh implementation; no reference source is copied.

#pragma once

#include "../../../Protocols/AVC/Core/IAvcUnit.hpp"
#include "../../../Protocols/AVC/Discovery/DiscoverySnapshot.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ASFW::Audio::BeBoB {

struct ChannelPosition {
    // Both fields are zero-based in ASFW. BridgeCo encodes each as one-based.
    uint8_t streamPosition{0};
    uint8_t sectionLocation{0};
};

struct ChannelSection {
    // BridgeCo section type, e.g. 0x0a for MIDI. Remains unknown until the
    // independent section-info request succeeds.
    std::optional<uint8_t> type{};
    std::vector<ChannelPosition> positions{};
};

/// ISO plug 0's channel sections; playback is the unit ISO input plug.
struct ChannelSections {
    std::vector<ChannelSection> playback;
    std::vector<ChannelSection> capture;
};

inline constexpr uint8_t kMaxChannelSections = 16;

/// The channel-position reply payload (after the EXTENDED PLUG INFO header).
[[nodiscard]] std::optional<std::vector<ChannelSection>> ParseChannelPositionSections(
    std::span<const uint8_t> payload) noexcept;

/// Channel positions, then each section's type, for ISO plug 0 in both
/// directions (STATUS only). Holds the unit by LiveRef: a unit destroyed
/// meanwhile ends the probe without completing.
void ProbeChannelSections(ASFW::AVC::IAvcUnit& unit, uint64_t guid,
                          std::function<void(ChannelSections)> completion);

/// Formations of unit ISO plug 0 per direction (compound AM824 entries from
/// the snapshot) and the current rate both directions' signal formats agree
/// on (0 when they disagree or either is unknown).
[[nodiscard]] ASFW::AVC::DiscoveryEngine::ExtensionFacts BridgeCoFormationFacts(
    const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot);

/// True when the unit reports at least one ISO plug in each direction.
[[nodiscard]] bool HasDuplexIsoPlugPair(const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot);

} // namespace ASFW::Audio::BeBoB
