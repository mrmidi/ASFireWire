// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuFamilyAdapter.cpp - see MotuFamilyAdapter.hpp.

#include "MotuFamilyAdapter.hpp"

#include "../Protocols/DeviceProtocolChoice.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include "../Protocols/MOTU/MotuRegisters.hpp"
#include "../Wire/MOTU/MotuBlockLayout.hpp"
#include "../Model/RateConfiguration.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace ASFW::Audio::Host {

Model::ASFWAudioDevice MotuFamilyAdapter::BuildNubConfig(const Discovery::DeviceRecord& record,
                                                         const IDeviceProtocol& protocol) {
    // MOTU has no profile registry to consult. The device's geometry comes from its
    // own registers, so the nub is built from the hardware's answer rather than a
    // table keyed on model_id (which MOTU publishes as 0 anyway).
    Model::ASFWAudioDevice dev{};
    dev.guid = record.guid;
    dev.vendorId = record.vendorId;
    dev.modelId = record.modelId;
    // CoreAudio shows this in the Sound panel, where MOTU's own driver named the
    // device "MOTU UltraLite". The model constants stay bare; only the display
    // name is qualified here.
    const char* const modelName =
        DeviceProfiles::Audio::AudioDeviceCatalog::MotuModelNameForSwVersion(
            record.unitSwVersion.value_or(0U));
    dev.deviceName = modelName != nullptr
                         ? std::string(DeviceProfiles::Audio::kMotuVendorName) + " " + modelName
                         : protocol.GetName();
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";
    dev.sampleRates = {44100, 48000}; // first-read failure fallback only
    dev.currentSampleRate = 48000u;

    // Geometry: the counts the protocol read from the optical config, or its
    // fixed table when that read failed (14 PCM chunks per direction at
    // 44.1/48 kHz, motu-protocol-v2.c:274-282).
    AudioStreamRuntimeCaps caps{};
    if (protocol.GetRuntimeAudioStreamCaps(caps) && caps.sampleRateHz != 0) {
        dev.inputChannelCount = caps.hostInputPcmChannels;
        dev.outputChannelCount = caps.hostOutputPcmChannels;
        dev.currentSampleRate = caps.sampleRateHz;
    } else {
        const uint32_t fixedChunks = ::ASFW::Encoding::Motu::k828mk2FixedPcmChunks[0];
        dev.inputChannelCount = fixedChunks;
        dev.outputChannelCount = fixedChunks;
    }
    dev.channelCount = std::max(dev.inputChannelCount, dev.outputChannelCount);
    // Even the read-failure fallback must advertise its actual current mode.
    // Starting still re-reads the register and refuses mismatching geometry.
    if (const auto index = Encoding::Motu::RateToIndex(dev.currentSampleRate); index >= 0) {
        const auto first = static_cast<uint32_t>(index) & ~1U;
        dev.sampleRates = {Encoding::Motu::kClockRates[first], Encoding::Motu::kClockRates[first + 1]};
    }
    if (const auto formations = protocol.RateFormations(); formations && !formations->empty()) {
        dev.rateFormationCandidates = *formations;
        dev.rateRouteIncarnation = record.deviceIncarnation;
        dev.rateRouteEpoch = record.routeEpoch;
        dev.rateBusGeneration = record.gen.value;
        dev.usesRateFormations = true;
        dev.deviceSampleRates = true;
        dev.sampleRates.clear();
        for (const auto& formation : *formations)
            if (formation.protocolSupported) dev.sampleRates.push_back(formation.sampleRateHz);
        const auto selected = Model::WithRateFormation(dev, dev.currentSampleRate);
        if (selected) dev = *selected;
        else dev.sampleRates.clear();
    }

    // Port names in host channel order, which is not wire order: the encoder and
    // decoder apply the same model table, so these line up with what each channel
    // carries.
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    if (protocol.GetChannelLabels(inNames, outNames)) {
        dev.inputChannelNames = std::move(inNames);
        dev.outputChannelNames = std::move(outNames);
    }
    return dev;
}

void MotuFamilyAdapter::Describe(const DescribeInput& in, DescribeDone done) {
    // The host only routes MOTU devices here; refuse anything else rather than
    // publish a MOTU description for another family's device.
    if (!in.policy || ChooseAudioBackend(in.policy->plan) != AudioBackendKind::MotuRegister) {
        done(DescribeRefusal{kIOReturnUnsupported, "not-a-motu-policy"});
        return;
    }
    // MotuAudioBackend published only once a protocol existed; the next record
    // update or restart describes again.
    if (!in.protocol) {
        done(DescribeRefusal{kIOReturnNotReady, "no-protocol"});
        return;
    }

    // Read the optical config first (asynchronous, as DICE loads its caps). The
    // lambda keeps the protocol alive until the read completes.
    const bool nubIsLive = in.committed.has_value();
    auto protocol = in.protocol;
    protocol->EnsureRuntimeStreamGeometry(
        [record = in.record, protocol, nubIsLive, done = std::move(done)](IOReturn geometryStatus) {
            if (geometryStatus == kIOReturnSuccess) {
                auto config = BuildNubConfig(record, *protocol);
                if (config.sampleRates.empty()) {
                    done(DescribeRefusal{kIOReturnUnsupported, "unusable-motu-formation"});
                    return;
                }
                done(std::move(config));
                return;
            }
            if (nubIsLive || record.unitSwVersion.value_or(0) == 0x15) {
                // The live nub's counts came from an earlier read. Building from the
                // fixed table now would differ from them and latch "geometry changed"
                // on what may be one lost transaction; refuse, and the next trigger
                // reads again.
                done(DescribeRefusal{geometryStatus, kReadFailedReason});
                return;
            }
            // First publication: the device must appear, so fall back to the model's
            // fixed geometry, and say so in the host's line.
            done(DescribedWithNote{BuildNubConfig(record, *protocol), kFixedGeometryNote});
        });
}

} // namespace ASFW::Audio::Host
