// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.hpp
// Global profile registry dispatcher.
//
// Static profiles (Phase88, Apogee) are returned as singletons. Dynamic profiles
// (generic BeBoB) are constructed from discovery data and owned per-GUID so that
// two different BeBoB devices can report different stream geometries.

#pragma once

#include "IAudioDeviceProfile.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace ASFW::Isoch::Audio {

class AudioProfileRegistry {
public:
    [[nodiscard]] static const IAudioDeviceProfile* FindProfile(uint32_t vendorId,
                                                                uint32_t modelId,
                                                                uint64_t guid) noexcept;

    // Create and store a per-GUID BeBoB profile from discovery data. Returns
    // the stored pointer (owned by the registry). No-op if already registered.
    static const IAudioDeviceProfile* RegisterBeBoBProfile(uint64_t guid,
                                                            const void* discoveryModel) noexcept;

    // Drop a per-GUID profile (device removed).
    static void UnregisterProfile(uint64_t guid) noexcept;

private:
    static std::unordered_map<uint64_t, std::unique_ptr<IAudioDeviceProfile>>& DynamicProfiles();
};

} // namespace ASFW::Isoch::Audio
