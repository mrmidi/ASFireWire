// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcExtensionInventory.cpp - The read-only vendor inventories that follow
// generic AV/C discovery.

#include "AvcExtensionInventory.hpp"
#include "../BeBoB/BeBoBCaptureChannelMap.hpp"

#include "../../Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"
#include "../../Protocols/Oxford/OxfwStreamFormats.hpp"
#include "../../../Logging/Logging.hpp"

#include <vector>

namespace ASFW::Protocols::AVC {

// The chip's read-only extension inventory, run by the unit after its generic
// discovery at attach and on every refresh. Each reaches the unit through
// IAvcUnit by LiveRef (never ownership), so replay can run it against a recorded
// unit; results are logged and recorded in the unit's exchange log.
AVCUnit::DiscoveryOptions DiscoveryOptionsFor(AvcExtensionInventory inventory) {
    AVCUnit::DiscoveryOptions options;
    switch (inventory) {
        case AvcExtensionInventory::kBridgeCo:
            // BridgeCo is 0x2F-only: Linux bebob_command.c sends only 0x2F, FFADO
            // sends only 0x2F on every BeBoB plug (avc_extended_stream_format.cpp:296),
            // and a Phase 88 answers 0xBF NOT IMPLEMENTED.
            options.streamFormatOpcode = ASFW::AVC::IAvcUnit::StreamFormatOpcodePolicy::kSupportOnly;
            options.extensionInventory = [](ASFW::AVC::IAvcUnit& unit, std::function<void(ASFW::AVC::DiscoveryEngine::ExtensionFacts)> done) {
                const uint64_t guid = unit.Guid();
                ::ASFW::Audio::BeBoB::StartBeBoBPlug0Discovery(
                    unit, guid,
                    [guid, done = std::move(done)](
                        const ::ASFW::Audio::BeBoB::DeviceModel& model) {
                        // BridgeCo "input" is the unit ISO input plug: host playback.
                        const auto formations = [](const ::ASFW::Audio::BeBoB::IsochronousPlugModel& plug) {
                            decltype(ASFW::AVC::DiscoveryEngine::ExtensionPlug::formations) out;
                            for (const auto& formation : plug.supportedFormations) {
                                const auto hz = formation.RateHz();
                                if (hz && !out.push_back({.rateHz = *hz, .pcmChannels = formation.pcmChannels,
                                                          .midiChannels = formation.midiSlots})) break; // Full.
                            }
                            return out;
                        };
                        const auto playback = formations(model.input);
                        const auto capture = formations(model.output);
                        const uint32_t rate = model.CurrentRateHz().value_or(0);
                        ASFW_LOG(AVC, "[AvcInventory] guid=%llx bridgeco plugCounts=%d formations in=%zu out=%zu rate=%u",
                                 guid, model.unitPlugCounts.has_value() ? 1 : 0, playback.size(), capture.size(), rate);
                        ASFW::AVC::DiscoveryEngine::ExtensionFacts facts;
                        facts.playback.formations = playback; facts.capture.formations = capture;
                        facts.playback.currentRateHz = rate; facts.capture.currentRateHz = rate;
                        for (const auto& f : playback) if (f.rateHz == rate)
                            facts.playback.pcmSlots = ::ASFW::Audio::BeBoBProbe::PlaybackChannelMapFromProbe(model.input, f.pcmChannels, f.pcmChannels + f.midiChannels);
                        for (const auto& f : capture) if (f.rateHz == rate)
                            facts.capture.pcmSlots = ::ASFW::Audio::BeBoBProbe::ChannelMapFromProbe(model.output, f.pcmChannels, f.pcmChannels + f.midiChannels);
                        done(std::move(facts));
                    });
            };
            break;
        case AvcExtensionInventory::kOxford:
            options.extensionInventory = [](ASFW::AVC::IAvcUnit& unit, std::function<void(ASFW::AVC::DiscoveryEngine::ExtensionFacts)> done) {
                const uint64_t guid = unit.Guid();
                ::ASFW::Audio::Oxford::DetectStreamFormats(
                    unit, /*isOutput=*/false,
                    [liveUnit = ASFW::Common::LiveRef<ASFW::AVC::IAvcUnit>(unit), guid, done = std::move(done)](
                        IOReturn inStatus, const ::ASFW::Audio::Oxford::StreamFormatSet& in) mutable {
                        auto* unit = liveUnit.Get();
                        if (!unit) return; // Unit gone: its session went with it.
                        const size_t inRates = in.Rates().size();
                        ::ASFW::Audio::Oxford::DetectStreamFormats(
                            *unit, /*isOutput=*/true,
                            [guid, inStatus, inRates, done = std::move(done)](
                                IOReturn outStatus, const ::ASFW::Audio::Oxford::StreamFormatSet& out) {
                                ASFW_LOG(AVC, "[AvcInventory] guid=%llx oxford formats in=0x%x/%zu out=0x%x/%zu",
                                         guid, inStatus, inRates, outStatus, out.Rates().size());
                                done({});
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
