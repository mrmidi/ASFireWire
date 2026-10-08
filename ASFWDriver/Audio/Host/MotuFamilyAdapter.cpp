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
    // Geometry combines the model table with completed clock/optical reads.
    // Model identity comes from Unit_Sw_Version; root model_id is generally 0.
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
    AudioStreamRuntimeCaps caps{};
    if (!protocol.GetRuntimeAudioStreamCaps(caps) || caps.sampleRateHz == 0) return dev;
    dev.inputChannelCount = caps.hostInputPcmChannels;
    dev.outputChannelCount = caps.hostOutputPcmChannels;
    dev.currentSampleRate = caps.sampleRateHz;
    dev.channelCount = std::max(dev.inputChannelCount, dev.outputChannelCount);
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
    auto protocol = in.protocol;
    protocol->EnsureRuntimeStreamGeometry(
        [record = in.record, protocol, done = std::move(done)](IOReturn geometryStatus) {
            if (geometryStatus == kIOReturnSuccess) {
                auto config = BuildNubConfig(record, *protocol);
                if (config.sampleRates.empty()) {
                    done(DescribeRefusal{kIOReturnUnsupported, "unusable-motu-formation"});
                    return;
                }
                done(std::move(config));
                return;
            }
            // Failed reads never authorize guessed geometry, including first
            // publication. Discovery can retry when the route is usable again.
            done(DescribeRefusal{geometryStatus, kReadFailedReason});
        });
}

} // namespace ASFW::Audio::Host
