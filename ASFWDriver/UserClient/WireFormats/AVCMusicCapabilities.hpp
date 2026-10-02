// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCMusicCapabilities.hpp - The user client's music-subunit capabilities blob,
// built from the immutable discovery snapshot.
//
// Layout: Shared/SharedDataModels.hpp (AVCMusicCapabilitiesWire, PlugInfoWire,
// SignalBlockWire, SupportedFormatWire), unchanged for the app. The content
// rules are the ones the app has always received:
// - Plugs, names and types come from the music subunit status descriptor.
// - A plug's signal blocks come from its current stream format when that is a
//   compound AM824 format; descriptor clusters only feed the channel maxima.
//   No ChannelDetailWire is emitted (numChannelDetails is always 0).
// - Supported formats are the plug's formation list, at most 32 per plug.
// - Rates are TA 2001002 codes; a code outside that table, or a non-compound
//   format, is 0xFF (unknown).
// - Plugs stop before the blob would exceed `maxBytes`.

#pragma once

#include "../../Protocols/AVC/Discovery/DiscoverySnapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ASFW::UserClient::Wire {

/// nullopt when the snapshot has no such subunit.
[[nodiscard]] std::optional<std::vector<uint8_t>> BuildMusicCapabilities(
    const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot, ASFW::AVC::SubunitId subunit,
    size_t maxBytes);

} // namespace ASFW::UserClient::Wire
