// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.cpp - Factory for creating device-specific protocol handlers

#include "DeviceProtocolFactory.hpp"
#include "DICE/Focusrite/SPro24DspProtocol.hpp"
#include "DICE/TCAT/DICETcatProtocol.hpp"
#include "Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "BeBoB/Phase88Protocol.hpp"
#include "BeBoB/GenericBeBoBProtocol.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Audio {

std::unique_ptr<IDeviceProtocol> DeviceProtocolFactory::Create(
    uint32_t vendorId,
    uint32_t modelId,
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    uint16_t nodeId,
    uint64_t deviceGuid,
    IRM::IRMClient* irmClient,
    CMP::CMPClient* cmpClient,
    Scheduling::ITimerScheduler* timerScheduler
) {
    if (vendorId == kFocusriteVendorId) {
        if (modelId == kSPro24DspModelId) {
            ASFW_LOG(DICE, "Creating SPro24DspProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                     vendorId, modelId, nodeId);
            return std::make_unique<DICE::Focusrite::SPro24DspProtocol>(busOps, busInfo, nodeId, irmClient);
        }

        if (modelId == kSPro14ModelId || modelId == kSPro24ModelId) {
            const auto known = LookupKnownIdentity(vendorId, modelId);
            ASFW_LOG(DICE,
                     "Creating generic DICETcatProtocol for %{public}s vendor=0x%06x model=0x%06x node=0x%04x",
                     (known.has_value() && known->modelName) ? known->modelName : "Focusrite DICE",
                     vendorId,
                     modelId,
                     nodeId);
            return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, nodeId, irmClient);
        }
    }

    if (vendorId == kAlesisVendorId && modelId == kAlesisMultiMixModelId) {
        ASFW_LOG(DICE,
                 "Creating generic DICETcatProtocol for Alesis MultiMix vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId,
                 modelId,
                 nodeId);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, nodeId, irmClient);
    }

    if (vendorId == kMidasVendorId && modelId == kMidasVeniceModelId) {
        ASFW_LOG(DICE,
                 "Creating generic DICETcatProtocol for Midas Venice vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId,
                 modelId,
                 nodeId);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, nodeId, irmClient);
    }

    if (vendorId == kPreSonusVendorId && modelId == kStudioLive1602ModelId) {
        ASFW_LOG(DICE,
                 "Creating generic DICETcatProtocol for PreSonus StudioLive 16.0.2 vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId,
                 modelId,
                 nodeId);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, nodeId, irmClient);
    }

    // Check for Apogee Duet FireWire (AV/C + vendor-dependent commands).
    if (vendorId == kApogeeVendorId && modelId == kApogeeDuetModelId) {
        ASFW_LOG(Audio,
                 "Creating ApogeeDuetProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId, modelId, nodeId);
        // Factory path intentionally does not bind FCP transport yet.
        // AVCDiscovery wires transport for live command execution.
        return std::make_unique<Oxford::Apogee::ApogeeDuetProtocol>(
            busOps, busInfo, nodeId, nullptr, irmClient, cmpClient, deviceGuid);
    }

    if (vendorId == kTerraTecVendorId && modelId == kPhase88RackFwModelId) {
        ASFW_LOG(Audio,
                 "Creating Phase88Protocol BeBoB/CMP backend vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId, modelId, nodeId);
        return std::make_unique<BeBoB::Phase88Protocol>(busOps, busInfo, nodeId, irmClient,
                                                        cmpClient, deviceGuid, timerScheduler);
    }
    // Known BeBoB device without a verified custom protocol: generic fallback.
    // Conservative defaults — plug-0, CMP, no mixer programming. Discovery model
    // wiring (for derived geometry) lands with the per-GUID profile work.
    if (DeviceProfiles::Audio::BeBoB::IsBeBoBDevice(vendorId, modelId)) {
        ASFW_LOG(Audio,
                 "Creating GenericBeBoBProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId, modelId, nodeId);
        return std::make_unique<BeBoB::GenericBeBoBProtocol>(
            busOps, busInfo, nodeId, irmClient, cmpClient, deviceGuid, timerScheduler,
            BeBoB::DeviceModel{});
    }

    // Unknown device
    return nullptr;
}

} // namespace ASFW::Audio
