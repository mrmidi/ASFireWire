// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SaffireproCommon.cpp - Common Saffire Pro implementations

#include "SaffireproCommon.hpp"
#include "../Core/DICETransaction.hpp"

namespace ASFW::Audio::DICE::Focusrite {

// ============================================================================
// InputParams
// ============================================================================

InputParams InputParams::FromWire(const uint8_t* data) {
    InputParams p;
    
    // Mic levels: bytes 0-1
    p.micLevels[0] = static_cast<MicInputLevel>(data[0]);
    p.micLevels[1] = static_cast<MicInputLevel>(data[1]);
    
    // Line levels: bytes 2-3
    p.lineLevels[0] = static_cast<LineInputLevel>(data[2]);
    p.lineLevels[1] = static_cast<LineInputLevel>(data[3]);
    
    return p;
}

void InputParams::ToWire(uint8_t* data) const {
    data[0] = static_cast<uint8_t>(micLevels[0]);
    data[1] = static_cast<uint8_t>(micLevels[1]);
    data[2] = static_cast<uint8_t>(lineLevels[0]);
    data[3] = static_cast<uint8_t>(lineLevels[1]);
    data[4] = 0;  // Reserved
    data[5] = 0;
    data[6] = 0;
    data[7] = 0;
}

// ============================================================================
// OutputGroupState
// ============================================================================

OutputGroupState OutputGroupState::FromWire(const uint8_t* data, size_t entryCount) {
    OutputGroupState s;
    
    // First quadlet: mute/dim status
    uint32_t status = DICETransaction::QuadletFromWire(data);
    s.muteEnabled = (status & 0x01) != 0;
    s.dimEnabled  = (status & 0x02) != 0;
    
    // Second quadlet: hardware knob value
    s.hwKnobValue = static_cast<int8_t>(DICETransaction::QuadletFromWire(data + 4) & 0x7F);
    
    // Per-output entries start at offset 8
    // Each entry: 4 bytes (volume, flags)
    for (size_t i = 0; i < entryCount && i < 6; ++i) {
        size_t offset = 8 + i * 8;
        
        uint32_t volData = DICETransaction::QuadletFromWire(data + offset);
        s.volumes[i] = static_cast<int8_t>(volData & 0x7F);
        s.volMutes[i] = (volData & 0x80) != 0;
        
        uint32_t flags = DICETransaction::QuadletFromWire(data + offset + 4);
        s.volHwCtls[i]  = (flags & 0x01) != 0;
        s.muteHwCtls[i] = (flags & 0x02) != 0;
        s.dimHwCtls[i]  = (flags & 0x04) != 0;
    }
    
    return s;
}

void OutputGroupState::ToWire(uint8_t* data) const {
    // First quadlet: mute/dim status
    uint32_t status = 0;
    if (muteEnabled) status |= 0x01;
    if (dimEnabled)  status |= 0x02;
    DICETransaction::QuadletToWire(status, data);
    
    // Second quadlet: hardware knob value
    DICETransaction::QuadletToWire(static_cast<uint32_t>(hwKnobValue) & 0x7F, data + 4);
    
    // Per-output entries
    for (size_t i = 0; i < 6; ++i) {
        size_t offset = 8 + i * 8;
        
        uint32_t volData = static_cast<uint32_t>(volumes[i]) & 0x7F;
        if (volMutes[i]) volData |= 0x80;
        DICETransaction::QuadletToWire(volData, data + offset);
        
        uint32_t flags = 0;
        if (volHwCtls[i])  flags |= 0x01;
        if (muteHwCtls[i]) flags |= 0x02;
        if (dimHwCtls[i])  flags |= 0x04;
        DICETransaction::QuadletToWire(flags, data + offset + 4);
    }
}

} // namespace ASFW::Audio::DICE::Focusrite
