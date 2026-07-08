// PreSonusAudioProfiles.hpp - PreSonus FireWire audio device knowledge (DICE/TCAT).
// Knows ONLY PreSonus devices; performs no runtime protocol construction.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::PreSonus {

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kPreSonusVendorId && query.modelId == kStudioLive1602ModelId) {
        return DeviceIdentityHint{.vendorId = query.vendorId,
                                  .modelId = query.modelId,
                                  .vendorName = kPreSonusVendorName,
                                  .modelName = kStudioLive1602ModelName,
                                  .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kPreSonusVendorId && query.modelId == kStudioLive1602ModelId) {
        // Identity captured live from the hardware (2026-07-08): GUID
        // 0x000A920404FE2011, unit directory specifier 0x000A92 version 0x000001.
        // Cross-checked with libffado 2.5.0 device config (vendor 0x000a92,
        // model 0x000013, driver DICE) and Linux snd-dice (generic path, no quirks).
        return AudioProfileHint{.family = AudioProtocolFamily::DICE,
                                .mode = AudioIntegrationMode::kHardcodedNub,
                                .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::PreSonus
