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
    if (query.vendorId == kPreSonusVendorId && query.modelId == kStudioLive2442ModelId) {
        // Geometry captured live from hardware in issue #115 (2026-09-14): GUID
        // 0x000A9204049204CB, TCAT product 0x012, two isochronous streams per
        // direction with an ASYMMETRIC playback side (16 + 10) — see
        // PreSonusStudioLive2442Profile, which owns the matching wire geometry.
        // Generic DICE path: no quirk in snd-dice (dice-presonus.c covers model
        // 0x000008 only) and none in libffado 2.5.0.
        return AudioProfileHint{.family = AudioProtocolFamily::DICE,
                                .mode = AudioIntegrationMode::kHardcodedNub,
                                .source = MatchSource::VendorModel};
    }
    // StudioLive 16.4.2 / 32.4.2 intentionally return no profile: their TX/RX
    // channel counts have not been captured from hardware, and a wrong DBS means
    // the device never locks to the host stream. Note for whenever a 16.4.2
    // capture arrives: libffado 2.5.0 records xmit_transfer_delay = 4 for it
    // (model 0x000010), the only StudioLive with any quirk at all — a register
    // dump will never reveal that.
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::PreSonus
