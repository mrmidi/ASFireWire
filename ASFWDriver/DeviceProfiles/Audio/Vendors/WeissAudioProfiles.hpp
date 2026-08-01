// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// WeissAudioProfiles.hpp - Weiss Engineering FireWire identity and integration metadata.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::Weiss {

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId != kWeissVendorId) {
        return std::nullopt;
    }

    const char* modelName = nullptr;
    switch (query.modelId) {
        case kWeissAdc2ModelId:   modelName = kWeissAdc2ModelName; break;
        case kWeissVestaModelId:  modelName = kWeissVestaModelName; break;
        case kWeissDac2ModelId:   modelName = kWeissDac2ModelName; break;
        case kWeissAfi1ModelId:   modelName = kWeissAfi1ModelName; break;
        case kWeissInt202ModelId: modelName = kWeissInt202ModelName; break;
        case kWeissDac202ModelId: modelName = kWeissDac202ModelName; break;
        case kWeissMayaModelId:   modelName = kWeissMayaModelName; break;
        case kWeissInt203ModelId: modelName = kWeissInt203ModelName; break;
        case kWeissMan301ModelId: modelName = kWeissMan301ModelName; break;
        default:                   return std::nullopt;
    }

    return DeviceIdentityHint{.vendorId = query.vendorId,
                              .modelId = query.modelId,
                              .vendorName = kWeissVendorName,
                              .modelName = modelName,
                              .source = MatchSource::VendorModel};
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId != kWeissVendorId) {
        return std::nullopt;
    }

    switch (query.modelId) {
        // These two models use the DICE 2-channel TX/RX stream layout in
        // Linux. INT202's DICE capture section remains operationally duplex,
        // but is deliberately hidden from CoreAudio by its runtime policy.
        case kWeissInt202ModelId:
        case kWeissInt203ModelId:
            return AudioProfileHint{.family = AudioProtocolFamily::DICE,
                                    .mode = AudioIntegrationMode::kHardcodedNub,
                                    .source = MatchSource::VendorModel};

        // Identity is useful now, but audio enablement requires a verified
        // stream/clock trace for the individual model.
        case kWeissAdc2ModelId:
        case kWeissVestaModelId:
        case kWeissDac2ModelId:
        case kWeissAfi1ModelId:
        case kWeissDac202ModelId:
        case kWeissMayaModelId:
        case kWeissMan301ModelId:
            return AudioProfileHint{.family = AudioProtocolFamily::DICE,
                                    .mode = AudioIntegrationMode::kNone,
                                    .source = MatchSource::VendorModel};
        default:
            return std::nullopt;
    }
}

} // namespace ASFW::DeviceProfiles::Audio::Weiss
