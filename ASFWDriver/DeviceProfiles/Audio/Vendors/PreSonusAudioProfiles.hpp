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
    if (query.vendorId != kPreSonusVendorId) {
        return std::nullopt;
    }
    // StudioLive siblings are recognized by name only until their stream geometry
    // is captured from hardware — see LookupAudioProfile.
    const char* modelName = nullptr;
    switch (query.modelId) {
    case kStudioLive1602ModelId: modelName = kStudioLive1602ModelName; break;
    case kStudioLive1642ModelId: modelName = kStudioLive1642ModelName; break;
    case kStudioLive2442ModelId: modelName = kStudioLive2442ModelName; break;
    case kStudioLive3242ModelId: modelName = kStudioLive3242ModelName; break;
    default: return std::nullopt;
    }
    return DeviceIdentityHint{.vendorId = query.vendorId,
                              .modelId = query.modelId,
                              .vendorName = kPreSonusVendorName,
                              .modelName = modelName,
                              .source = MatchSource::VendorModel};
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
    // StudioLive 16.4.2 / 24.4.2 / 32.4.2 intentionally return no profile: their
    // TX/RX channel counts differ from the 16.0.2 and have not been captured from
    // hardware, and a wrong DBS means the device never locks to the host stream.
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::PreSonus
