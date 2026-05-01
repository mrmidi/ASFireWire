// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.cpp - Factory for creating device-specific protocol handlers

#include "DeviceProtocolFactory.hpp"
#include "DICE/Focusrite/SPro24DspProtocol.hpp"
#include "DICE/TCAT/DICETcatProtocol.hpp"
#include "Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Audio {

std::unique_ptr<IDeviceProtocol> DeviceProtocolFactory::Create(
    uint32_t vendorId,
    uint32_t modelId,
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    uint16_t nodeId,
    IRM::IRMClient* irmClient
) {
    const uint32_t normalizedVendorId = Normalize24(vendorId);
    const uint32_t normalizedModelId = Normalize24(modelId);

    if (normalizedVendorId == kFocusriteVendorId) {
        if (normalizedModelId == kSPro24DspModelId) {
            ASFW_LOG(DICE, "Creating SPro24DspProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                     normalizedVendorId, normalizedModelId, nodeId);
            return std::make_unique<DICE::Focusrite::SPro24DspProtocol>(busOps, busInfo, nodeId, irmClient);
        }

        if (normalizedModelId == kSPro14ModelId || normalizedModelId == kSPro24ModelId) {
            const auto known = LookupKnownIdentity(normalizedVendorId, normalizedModelId);
            ASFW_LOG(DICE,
                     "Creating generic DICETcatProtocol for %{public}s vendor=0x%06x model=0x%06x node=0x%04x",
                     (known.has_value() && known->modelName) ? known->modelName : "Focusrite DICE",
                     normalizedVendorId,
                     normalizedModelId,
                     nodeId);
            return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, nodeId, irmClient);
        }
    }

    if (normalizedVendorId == kAlesisVendorId && normalizedModelId == kAlesisMultiMixModelId) {
        ASFW_LOG(DICE,
                 "Creating generic DICETcatProtocol for Alesis MultiMix vendor=0x%06x model=0x%06x node=0x%04x",
                 normalizedVendorId,
                 normalizedModelId,
                 nodeId);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, nodeId, irmClient);
    }

    if (normalizedVendorId == kMidasVendorId && normalizedModelId == kMidasVeniceF32ModelId) {
        ASFW_LOG(DICE,
                 "Creating generic DICETcatProtocol for %{public}s vendor=0x%06x model=0x%06x node=0x%04x",
                 kMidasVeniceF32ModelName,
                 normalizedVendorId,
                 normalizedModelId,
                 nodeId);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, nodeId, irmClient);
    }

    // Check for Apogee Duet FireWire (AV/C + vendor-dependent commands).
    if (normalizedVendorId == kApogeeVendorId && normalizedModelId == kApogeeDuetModelId) {
        ASFW_LOG(Audio,
                 "Creating ApogeeDuetProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                 normalizedVendorId, normalizedModelId, nodeId);
        // Factory path intentionally does not bind FCP transport yet.
        // AVCDiscovery wires transport for live command execution.
        return std::make_unique<Oxford::Apogee::ApogeeDuetProtocol>(busOps, busInfo, nodeId, nullptr);
    }

    if (const auto known = LookupKnownIdentity(normalizedVendorId, normalizedModelId); known.has_value()) {
        ASFW_LOG(Audio,
                 "Recognized FireWire audio metadata only: vendor=0x%06x model=0x%06x name=\"%{public}s %{public}s\" family=%{public}s support=%{public}s source=%{public}s; no protocol handler will be created",
                 normalizedVendorId,
                 normalizedModelId,
                 known->vendorName ? known->vendorName : "",
                 known->modelName ? known->modelName : "",
                 ToString(known->protocolFamily),
                 ToString(known->supportStatus),
                 known->source ? known->source : "unknown");
        return nullptr;
    }
    
    // Unknown device
    return nullptr;
}

} // namespace ASFW::Audio
