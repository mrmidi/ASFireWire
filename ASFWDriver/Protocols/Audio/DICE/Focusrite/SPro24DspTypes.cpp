// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SPro24DspTypes.cpp - Wire serialization for Saffire Pro 24 DSP-only types

#include "SPro24DspTypes.hpp"
#include "../../../../Common/WireFormat.hpp"

namespace ASFW::Audio::DICE::Focusrite {

namespace {

float FloatFromWire(const uint8_t* data) noexcept {
    const uint32_t bits = ASFW::FW::ReadBE32(data);
    float f;
    static_assert(sizeof(float) == sizeof(uint32_t));
    __builtin_memcpy(&f, &bits, sizeof(float));
    return f;
}

void FloatToWire(float value, uint8_t* data) noexcept {
    uint32_t bits;
    static_assert(sizeof(float) == sizeof(uint32_t));
    __builtin_memcpy(&bits, &value, sizeof(float));
    ASFW::FW::WriteBE32(data, bits);
}

} // namespace

// ============================================================================
// CompressorState
// ============================================================================

CompressorState CompressorState::Deserialize(const uint8_t* data) {
    CompressorState s;

    // Per Linux reference (spro24dsp.rs), quad 0 (offset 0x00) is reserved
    // (always 0x3f800000 = 1.0f). Actual coefficients start at offset 0x04.
    for (size_t ch = 0; ch < 2; ++ch) {
        const uint8_t* block = data + ch * kCoefBlockSize;
        s.output[ch]    = FloatFromWire(block + 0x04);
        s.threshold[ch] = FloatFromWire(block + 0x08);
        s.ratio[ch]     = FloatFromWire(block + 0x0C);
        s.attack[ch]    = FloatFromWire(block + 0x10);
        s.release[ch]   = FloatFromWire(block + 0x14);
    }

    return s;
}

void CompressorState::Serialize(uint8_t* data) const {
    // Per Linux reference (spro24dsp.rs), quad 0 (offset 0x00) is reserved.
    // Write 1.0f to the reserved field, then actual coefficients at 0x04+.
    for (size_t ch = 0; ch < 2; ++ch) {
        uint8_t* block = data + ch * kCoefBlockSize;
        FloatToWire(1.0f,          block + 0x00);  // reserved (always 1.0)
        FloatToWire(output[ch],    block + 0x04);
        FloatToWire(threshold[ch], block + 0x08);
        FloatToWire(ratio[ch],     block + 0x0C);
        FloatToWire(attack[ch],    block + 0x10);
        FloatToWire(release[ch],   block + 0x14);
    }
}

// ============================================================================
// ReverbState
// ============================================================================

ReverbState ReverbState::Deserialize(const uint8_t* data) {
    ReverbState s;
    s.size = FloatFromWire(data + 0x70);
    s.air  = FloatFromWire(data + 0x74);

    const float on = FloatFromWire(data + 0x78);
    s.enabled = on > 0.5f;

    const float mag  = FloatFromWire(data + 0x80);
    const float sign = FloatFromWire(data + 0x84);
    s.preFilter = (sign >= 0.5f) ? mag : -mag;

    return s;
}

void ReverbState::Serialize(uint8_t* data) const {
    FloatToWire(size,                                  data + 0x70);
    FloatToWire(air,                                   data + 0x74);
    FloatToWire(enabled ? 1.0f : 0.0f,                data + 0x78);
    FloatToWire(enabled ? 0.0f : 1.0f,                data + 0x7C);
    FloatToWire((preFilter < 0.0f) ? -preFilter : preFilter, data + 0x80);
    FloatToWire((preFilter >= 0.0f) ? 1.0f : 0.0f,   data + 0x84);
}

// ============================================================================
// EffectGeneralParams
// ============================================================================

EffectGeneralParams EffectGeneralParams::Deserialize(const uint8_t* data) {
    EffectGeneralParams p;
    const uint32_t flags = ASFW::FW::ReadBE32(data);

    // Two-half-word layout per Linux reference (spro24dsp.rs):
    //   Ch0 in bits  0-2:  bit0 = EQ enable, bit1 = Comp enable, bit2 = EQ after comp
    //   Ch1 in bits 16-18: bit16= EQ enable, bit17= Comp enable, bit18= EQ after comp
    for (size_t ch = 0; ch < 2; ++ch) {
        const uint16_t chFlags = static_cast<uint16_t>(flags >> (ch * 16));
        p.eqEnable[ch]    = (chFlags & 0x0001) != 0;
        p.compEnable[ch]  = (chFlags & 0x0002) != 0;
        p.eqAfterComp[ch] = (chFlags & 0x0004) != 0;
    }

    return p;
}

void EffectGeneralParams::Serialize(uint8_t* data) const {
    uint32_t flags = 0;

    // Two-half-word layout per Linux reference (spro24dsp.rs):
    //   Ch0 in bits  0-2:  bit0 = EQ enable, bit1 = Comp enable, bit2 = EQ after comp
    //   Ch1 in bits 16-18: bit16= EQ enable, bit17= Comp enable, bit18= EQ after comp
    for (size_t ch = 0; ch < 2; ++ch) {
        uint16_t chFlags = 0;
        if (eqEnable[ch])    chFlags |= 0x0001;
        if (compEnable[ch])  chFlags |= 0x0002;
        if (eqAfterComp[ch]) chFlags |= 0x0004;
        flags |= static_cast<uint32_t>(chFlags) << (ch * 16);
    }

    ASFW::FW::WriteBE32(data, flags);
}

} // namespace ASFW::Audio::DICE::Focusrite
