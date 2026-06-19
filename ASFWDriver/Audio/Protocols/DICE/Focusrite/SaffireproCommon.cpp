// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SaffireproCommon.cpp - Common Saffire Pro implementations

#include "SaffireproCommon.hpp"
#include "../../../../Common/WireFormat.hpp"

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

InputParams InputParams::Deserialize(const uint8_t* data) {
    InputParams p;

    const uint32_t micFlags = ASFW::FW::ReadBE32(data);
    const uint32_t lineFlags = ASFW::FW::ReadBE32(data + 4);

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

void InputParams::Serialize(uint8_t* data) const {
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

    ASFW::FW::WriteBE32(data, micFlags);
    ASFW::FW::WriteBE32(data + 4, lineFlags);
}

// ============================================================================
// OutputGroupState
// ============================================================================

OutputGroupState OutputGroupState::Deserialize(const uint8_t* data) {
    OutputGroupState s;

    s.muteEnabled = ASFW::FW::ReadBE32(data) != 0;
    s.dimEnabled = ASFW::FW::ReadBE32(data + 4) != 0;

    for (size_t pair = 0; pair < kOutputPairCount; ++pair) {
        const size_t pos = 0x08 + pair * 4;
        const uint32_t packedVols = ASFW::FW::ReadBE32(data + pos);
        for (size_t lane = 0; lane < 2; ++lane) {
            const size_t index = pair * 2 + lane;
            const int8_t stored = static_cast<int8_t>((packedVols >> (lane * 8)) & 0xFFU);
            s.volumes[index] = static_cast<int8_t>(kVolMax - stored);
        }
    }

    for (size_t pair = 0; pair < kOutputPairCount; ++pair) {
        const size_t pos = 0x1C + pair * 4;
        const uint32_t flags = ASFW::FW::ReadBE32(data + pos);
        const size_t index = pair * 2;
        s.volHwCtls[index + 0] = (flags & (1U << 0)) != 0;
        s.volHwCtls[index + 1] = (flags & (1U << 1)) != 0;
        s.volMutes[index + 0] = (flags & (1U << 2)) != 0;
        s.volMutes[index + 1] = (flags & (1U << 3)) != 0;
    }

    const uint32_t dimMuteHw = ASFW::FW::ReadBE32(data + 0x30);
    for (size_t i = 0; i < s.muteHwCtls.size(); ++i) {
        s.muteHwCtls[i] = (dimMuteHw & (1U << i)) != 0;
        s.dimHwCtls[i] = (dimMuteHw & (1U << (i + 10))) != 0;
    }

    s.hwKnobValue = static_cast<int8_t>(static_cast<int32_t>(ASFW::FW::ReadBE32(data + 0x48)));
    return s;
}

void OutputGroupState::Serialize(uint8_t* data) const {
    for (size_t i = 0; i < kOutputGroupStateSize; ++i) {
        data[i] = 0;
    }

    ASFW::FW::WriteBE32(data, muteEnabled ? 1U : 0U);
    ASFW::FW::WriteBE32(data + 4, dimEnabled ? 1U : 0U);

    for (size_t pair = 0; pair < kOutputPairCount; ++pair) {
        const size_t index = pair * 2;
        const uint32_t packedVols =
            ClampVolumeToWire(volumes[index + 0]) |
            (ClampVolumeToWire(volumes[index + 1]) << 8);
        ASFW::FW::WriteBE32(data + 0x08 + pair * 4, packedVols);
    }

    for (size_t pair = 0; pair < kOutputPairCount; ++pair) {
        const size_t index = pair * 2;
        uint32_t flags = 0;
        if (volHwCtls[index + 0]) flags |= 1U << 0;
        if (volHwCtls[index + 1]) flags |= 1U << 1;
        if (volMutes[index + 0]) flags |= 1U << 2;
        if (volMutes[index + 1]) flags |= 1U << 3;
        ASFW::FW::WriteBE32(data + 0x1C + pair * 4, flags);
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
    ASFW::FW::WriteBE32(data + 0x30, dimMuteHw);

    ASFW::FW::WriteBE32(data + 0x48, static_cast<uint32_t>(static_cast<int32_t>(hwKnobValue)));
}

} // namespace ASFW::Audio::DICE::Focusrite
