// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcExtensionInventory.cpp - The read-only vendor inventories that follow
// generic AV/C discovery.

#include "AvcExtensionInventory.hpp"

#include "../../Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"
#include "../../Audio/Protocols/Oxford/OxfwStreamFormats.hpp"
#include "../../Logging/Logging.hpp"

#include <vector>

namespace ASFW::Protocols::AVC {

// The chip's read-only extension inventory, run by the unit after its generic
// discovery at attach and on every refresh. Each keeps the unit alive until it
// finishes; results are logged and recorded in the unit's exchange log.
AVCUnit::DiscoveryOptions DiscoveryOptionsFor(AvcExtensionInventory inventory) {
    AVCUnit::DiscoveryOptions options;
    switch (inventory) {
        case AvcExtensionInventory::kBridgeCo:
            options.extensionInventory = [](AVCUnit& unit, std::function<void()> done) {
                const uint64_t guid = unit.Guid();
                ::ASFW::Audio::BeBoB::StartBeBoBPlug0Discovery(
                    unit, guid,
                    [keepAlive = unit.shared_from_this(), guid, done = std::move(done)](
                        const ::ASFW::Audio::BeBoB::DeviceModel& model) {
                        // BridgeCo "input" is the unit ISO input plug: host playback.
                        const auto formations = [](const ::ASFW::Audio::BeBoB::IsochronousPlugModel& plug) {
                            std::vector<UnitPlugFormation> out;
                            for (const auto& formation : plug.supportedFormations) {
                                if (const auto hz = formation.RateHz()) {
                                    out.push_back({.rateHz = *hz, .pcmChannels = formation.pcmChannels,
                                                   .midiChannels = formation.midiSlots});
                                }
                            }
                            return out;
                        };
                        const auto playback = formations(model.input);
                        const auto capture = formations(model.output);
                        const uint32_t rate = model.CurrentRateHz().value_or(0);
                        ASFW_LOG(AVC, "[AvcInventory] guid=%llx bridgeco plugCounts=%d formations in=%zu out=%zu rate=%u",
                                 guid, model.unitPlugCounts.has_value() ? 1 : 0, playback.size(), capture.size(), rate);
                        keepAlive->CompleteGraphFromUnitPlugFormations(playback, capture, rate);
                        done();
                    });
            };
            break;
        case AvcExtensionInventory::kOxford:
            options.extensionInventory = [](AVCUnit& unit, std::function<void()> done) {
                const uint64_t guid = unit.Guid();
                auto keepAlive = unit.shared_from_this();
                ::ASFW::Audio::Oxford::DetectStreamFormats(
                    unit, /*isOutput=*/false,
                    [keepAlive, guid, done = std::move(done)](
                        IOReturn inStatus, const ::ASFW::Audio::Oxford::StreamFormatSet& in) mutable {
                        const size_t inRates = in.Rates().size();
                        ::ASFW::Audio::Oxford::DetectStreamFormats(
                            *keepAlive, /*isOutput=*/true,
                            [keepAlive, guid, inStatus, inRates, done = std::move(done)](
                                IOReturn outStatus, const ::ASFW::Audio::Oxford::StreamFormatSet& out) {
                                ASFW_LOG(AVC, "[AvcInventory] guid=%llx oxford formats in=0x%x/%zu out=0x%x/%zu",
                                         guid, inStatus, inRates, outStatus, out.Rates().size());
                                done();
                            });
                    });
            };
            break;
        case AvcExtensionInventory::kNone:
            break;
    }
    return options;
}

} // namespace ASFW::Protocols::AVC
