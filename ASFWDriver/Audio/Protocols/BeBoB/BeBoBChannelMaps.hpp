// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBChannelMaps.hpp - Per-device PCM channel -> AM824 slot maps, as the
// device reported them at discovery.
//
// BridgeCo firmware says where each PCM channel sits in the data block, and it
// need not be channel order: the Phase 88 is planar, odd channels then even,
// with S/PDIF first (measured 2026-09-27). Linux builds the same maps from the
// channel positions (bebob_stream.c:254-372).
//
// Discovery writes an entry (AVCDiscovery::PublishBeBoBAudioConfig); the
// capture side reads it when building the duplex profile, the playback side
// when configuring the TX engine. Both run in the one dext process. Header-only
// on purpose: the store is an inline function's static, one per process, so no
// extra translation unit has to be linked into every test that includes it.
//
// Concurrency: written at publish, read at stream start, after the nub exists.
// Same model as AudioProfileRegistry's per-GUID BeBoB profiles.

#pragma once

#include "../../Wire/AMDTP/PcmSlotMap.hpp"

#include <cstdint>
#include <unordered_map>

namespace ASFW::Audio::BeBoB {

/// Empty maps mean identity: PCM channel N in slot N.
struct DeviceChannelMaps {
    Wire::PcmSlotMap capture{};
    Wire::PcmSlotMap playback{};
};

namespace detail {
inline std::unordered_map<uint64_t, DeviceChannelMaps>& ChannelMapStore() {
    static std::unordered_map<uint64_t, DeviceChannelMaps> store;
    return store;
}
} // namespace detail

/// Replaces any earlier entry for the GUID.
inline void RegisterDeviceChannelMaps(uint64_t guid, const DeviceChannelMaps& maps) {
    if (guid != 0) {
        detail::ChannelMapStore()[guid] = maps;
    }
}

/// Identity maps when nothing is stored for the GUID.
[[nodiscard]] inline DeviceChannelMaps DeviceChannelMapsFor(uint64_t guid) {
    const auto& store = detail::ChannelMapStore();
    if (const auto it = store.find(guid); it != store.end()) {
        return it->second;
    }
    return {};
}

} // namespace ASFW::Audio::BeBoB
