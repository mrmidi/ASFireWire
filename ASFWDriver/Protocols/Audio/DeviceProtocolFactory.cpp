// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.cpp - Factory for creating device-specific protocol handlers

#include "DeviceProtocolFactory.hpp"
#include "DICE/Focusrite/SPro24DspProtocol.hpp"
#include "Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Audio {

std::unique_ptr<IDeviceProtocol> DeviceProtocolFactory::Create(
    uint32_t vendorId,
    uint32_t modelId,
    Async::AsyncSubsystem& subsystem,
    uint16_t nodeId
) {
    // Check for Focusrite Saffire Pro 24 DSP
    if (vendorId == kFocusriteVendorId && modelId == kSPro24DspModelId) {
        ASFW_LOG(DICE, "Creating SPro24DspProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId, modelId, nodeId);
        return std::make_unique<DICE::Focusrite::SPro24DspProtocol>(subsystem, nodeId);
    }

    // Check for Apogee Duet FireWire (AV/C + vendor-dependent commands).
    if (vendorId == kApogeeVendorId && modelId == kApogeeDuetModelId) {
        ASFW_LOG(Audio,
                 "Creating ApogeeDuetProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId, modelId, nodeId);
        // Factory path intentionally does not bind FCP transport yet.
        // AVCDiscovery wires transport for live command execution.
        return std::make_unique<Oxford::Apogee::ApogeeDuetProtocol>(subsystem, nodeId, nullptr);
    }
    
    // Unknown device
    return nullptr;
}

} // namespace ASFW::Audio
