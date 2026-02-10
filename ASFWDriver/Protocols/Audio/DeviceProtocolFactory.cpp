// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.cpp - Factory for creating device-specific protocol handlers

#include "DeviceProtocolFactory.hpp"
#include "DICE/Focusrite/SPro24DspProtocol.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Audio {

std::unique_ptr<IDeviceProtocol> DeviceProtocolFactory::Create(
    uint32_t vendorId,
    uint32_t modelId,
    Async::AsyncSubsystem& subsystem,
    uint16_t nodeId
) {
    using namespace DICE::Focusrite;
    
    // Check for Focusrite Saffire Pro 24 DSP
    if (vendorId == kFocusriteVendorId && modelId == kSPro24DspModelId) {
        ASFW_LOG(DICE, "Creating SPro24DspProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId, modelId, nodeId);
        return std::make_unique<SPro24DspProtocol>(subsystem, nodeId);
    }
    
    // Unknown device
    return nullptr;
}

bool DeviceProtocolFactory::IsKnownDevice(uint32_t vendorId, uint32_t modelId) {
    using namespace DICE::Focusrite;
    
    if (vendorId == kFocusriteVendorId && modelId == kSPro24DspModelId) {
        return true;
    }
    
    return false;
}

} // namespace ASFW::Audio
