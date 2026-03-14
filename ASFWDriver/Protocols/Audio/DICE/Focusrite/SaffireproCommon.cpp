// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SaffireproCommon.cpp - Common Saffire Pro implementations

#include "SaffireproCommon.hpp"
#include "../Core/DICETransaction.hpp"

namespace ASFW::Audio::DICE::Focusrite {

namespace {

constexpr size_t kOutputPairCount = 3;

uint32_t ClampVolumeToWire(int8_t logicalVolume) noexcept {
    int value = logicalVolume;
    if (value < OutputGroupState::kVolMin) {
        value = OutputGroupState::kVolMin;
    } else if (value > OutputGroupState::kVolMax) {
        value = OutputGroupState::kVolMax;
    }

    return static_cast<uint32_t>(OutputGroupState::kVolMax - static_cast<int8_t>(value));
}

} // namespace

// ============================================================================
// InputParams
// ============================================================================

InputParams InputParams::FromWire(const uint8_t* data) {
    InputParams p;

    const uint32_t micFlags = DICETransaction::QuadletFromWire(data);
    const uint32_t lineFlags = DICETransaction::QuadletFromWire(data + 4);

    for (size_t i = 0; i < p.micLevels.size(); ++i) {
        const uint16_t flags = static_cast<uint16_t>(micFlags >> (16 * i));
        p.micLevels[i] = (flags & 0x0002U) != 0
            ? MicInputLevel::Instrument
            : MicInputLevel::Line;
    }

    for (size_t i = 0; i < p.lineLevels.size(); ++i) {
        const uint16_t flags = static_cast<uint16_t>(lineFlags >> (16 * i));
        p.lineLevels[i] = (flags & 0x0001U) != 0
            ? LineInputLevel::High
            : LineInputLevel::Low;
    }

    return p;
}

void InputParams::ToWire(uint8_t* data) const {
    uint32_t micFlags = 0;
    for (size_t i = 0; i < micLevels.size(); ++i) {
        if (micLevels[i] == MicInputLevel::Instrument) {
            micFlags |= 0x0002U << (16 * i);
        }
    }

    uint32_t lineFlags = 0;
    for (size_t i = 0; i < lineLevels.size(); ++i) {
        if (lineLevels[i] == LineInputLevel::High) {
            lineFlags |= 0x0001U << (16 * i);
        }
    }

    DICETransaction::QuadletToWire(micFlags, data);
    DICETransaction::QuadletToWire(lineFlags, data + 4);
}

// ============================================================================
// OutputGroupState
// ============================================================================

OutputGroupState OutputGroupState::FromWire(const uint8_t* data) {
    OutputGroupState s;

    s.muteEnabled = DICETransaction::QuadletFromWire(data) != 0;
    s.dimEnabled = DICETransaction::QuadletFromWire(data + 4) != 0;

    for (size_t pair = 0; pair < kOutputPairCount; ++pair) {
        const size_t pos = 0x08 + pair * 4;
        const uint32_t packedVols = DICETransaction::QuadletFromWire(data + pos);
        for (size_t lane = 0; lane < 2; ++lane) {
            const size_t index = pair * 2 + lane;
            const int8_t stored = static_cast<int8_t>((packedVols >> (lane * 8)) & 0xFFU);
            s.volumes[index] = static_cast<int8_t>(kVolMax - stored);
        }
    }

    for (size_t pair = 0; pair < kOutputPairCount; ++pair) {
        const size_t pos = 0x1C + pair * 4;
        const uint32_t flags = DICETransaction::QuadletFromWire(data + pos);
        const size_t index = pair * 2;
        s.volHwCtls[index + 0] = (flags & (1U << 0)) != 0;
        s.volHwCtls[index + 1] = (flags & (1U << 1)) != 0;
        s.volMutes[index + 0] = (flags & (1U << 2)) != 0;
        s.volMutes[index + 1] = (flags & (1U << 3)) != 0;
    }

    const uint32_t dimMuteHw = DICETransaction::QuadletFromWire(data + 0x30);
    for (size_t i = 0; i < s.muteHwCtls.size(); ++i) {
        s.muteHwCtls[i] = (dimMuteHw & (1U << i)) != 0;
        s.dimHwCtls[i] = (dimMuteHw & (1U << (i + 10))) != 0;
    }

    s.hwKnobValue = static_cast<int8_t>(static_cast<int32_t>(DICETransaction::QuadletFromWire(data + 0x48)));
    return s;
}

void OutputGroupState::ToWire(uint8_t* data) const {
    for (size_t i = 0; i < kOutputGroupStateSize; ++i) {
        data[i] = 0;
    }

    DICETransaction::QuadletToWire(muteEnabled ? 1U : 0U, data);
    DICETransaction::QuadletToWire(dimEnabled ? 1U : 0U, data + 4);

    for (size_t pair = 0; pair < kOutputPairCount; ++pair) {
        const size_t index = pair * 2;
        const uint32_t packedVols =
            ClampVolumeToWire(volumes[index + 0]) |
            (ClampVolumeToWire(volumes[index + 1]) << 8);
        DICETransaction::QuadletToWire(packedVols, data + 0x08 + pair * 4);
    }

    for (size_t pair = 0; pair < kOutputPairCount; ++pair) {
        const size_t index = pair * 2;
        uint32_t flags = 0;
        if (volHwCtls[index + 0]) flags |= 1U << 0;
        if (volHwCtls[index + 1]) flags |= 1U << 1;
        if (volMutes[index + 0]) flags |= 1U << 2;
        if (volMutes[index + 1]) flags |= 1U << 3;
        DICETransaction::QuadletToWire(flags, data + 0x1C + pair * 4);
    }

    uint32_t dimMuteHw = 0;
    for (size_t i = 0; i < muteHwCtls.size(); ++i) {
        if (muteHwCtls[i]) {
            dimMuteHw |= 1U << i;
        }
        if (dimHwCtls[i]) {
            dimMuteHw |= 1U << (i + 10);
        }
    }
    DICETransaction::QuadletToWire(dimMuteHw, data + 0x30);

    DICETransaction::QuadletToWire(static_cast<uint32_t>(static_cast<int32_t>(hwKnobValue)), data + 0x48);
}

} // namespace ASFW::Audio::DICE::Focusrite
