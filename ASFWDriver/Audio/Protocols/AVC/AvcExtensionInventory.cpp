// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcExtensionInventory.cpp - The read-only vendor inventories that follow
// generic AV/C discovery.

#include "AvcExtensionInventory.hpp"
#include "../BeBoB/BeBoBCaptureChannelMap.hpp"
#include "../BeBoB/BridgeCoInventory.hpp"

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
            options.extensionInventory = [](ASFW::AVC::IAvcUnit& unit,
                                            ASFW::AVC::DiscoveryEngine::SnapshotLease discovered,
                                            std::function<void(ASFW::AVC::DiscoveryEngine::ExtensionFacts)> done) {
                namespace B = ::ASFW::Audio::BeBoB;
                const uint64_t guid = unit.Guid();
                if (!discovered || !B::HasDuplexIsoPlugPair(*discovered)) {
                    ASFW_LOG(AVC, "[AvcInventory] guid=%llx bridgeco: no duplex ISO plug pair", guid);
                    done({});
                    return;
                }
                // Formations and rate are what generic discovery read; only the
                // channel sections are BridgeCo-specific.
                auto facts = B::BridgeCoFormationFacts(*discovered);
                B::ProbeChannelSections(unit, guid, [guid, facts, done = std::move(done)](B::ChannelSections sections) mutable {
                    const uint32_t rate = facts.playback.currentRateHz;
                    for (const auto& f : facts.playback.formations) if (f.rateHz == rate)
                        facts.playback.pcmSlots = ::ASFW::Audio::BeBoBProbe::ChannelMapFromSections(
                            sections.playback, f.pcmChannels, f.pcmChannels + f.midiChannels);
                    for (const auto& f : facts.capture.formations) if (f.rateHz == rate)
                        facts.capture.pcmSlots = ::ASFW::Audio::BeBoBProbe::ChannelMapFromSections(
                            sections.capture, f.pcmChannels, f.pcmChannels + f.midiChannels);
                    ASFW_LOG(AVC, "[AvcInventory] guid=%llx bridgeco formations in=%zu out=%zu rate=%u sections in=%zu out=%zu",
                             guid, facts.playback.formations.size(), facts.capture.formations.size(), rate,
                             sections.playback.size(), sections.capture.size());
                    done(std::move(facts));
                });
            };
            break;
        case AvcExtensionInventory::kOxford:
            options.extensionInventory = [](ASFW::AVC::IAvcUnit& unit, ASFW::AVC::DiscoveryEngine::SnapshotLease,
                                            std::function<void(ASFW::AVC::DiscoveryEngine::ExtensionFacts)> done) {
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
