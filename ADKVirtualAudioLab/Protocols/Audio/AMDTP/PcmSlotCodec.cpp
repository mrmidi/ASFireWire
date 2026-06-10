#include "PcmSlotCodec.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

// Float [-1.0, +1.0] → signed 24-bit, symmetric scale by 2^23-1, round half
// away from zero. No libm dependency (DriverKit-safe).
//
// Encodings return logical quadlet values; serialization to bus byte order
// happens at payload write time. The LE variant pre-swaps so that big-endian
// serialization yields little-endian sample bytes inside the slot.

namespace {
constexpr float kScale24 = 8388607.0f; // 2^23 - 1

[[nodiscard]] constexpr uint32_t ByteSwap32(uint32_t v) noexcept {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}
} // namespace

int32_t PcmSlotCodec::Float32ToSigned24(float sample) noexcept {
    if (sample > 1.0f) {
        sample = 1.0f;
    } else if (sample < -1.0f) {
        sample = -1.0f;
    }
    const float scaled = sample * kScale24;
    return (scaled >= 0.0f) ? static_cast<int32_t>(scaled + 0.5f)
                            : static_cast<int32_t>(scaled - 0.5f);
}

uint32_t PcmSlotCodec::EncodeAm824MBLA(float sample) noexcept {
    // AM824 Multi-Bit Linear Audio: label 0x40 in [31:24], 24-bit two's
    // complement sample in [23:0] (IEC 61883-6).
    return 0x40000000u |
           (static_cast<uint32_t>(Float32ToSigned24(sample)) & 0x00FFFFFFu);
}

uint32_t PcmSlotCodec::EncodeRawSigned24In32BE(float sample) noexcept {
    // 24-bit sample sign-extended across the full 32-bit slot, no label byte.
    // Saffire.kext host→device wire capture shows negative samples with 0xFF
    // in [31:24] (e.g. 0xFFFC9F0C): the top byte is sign extension, not zero
    // padding. Masking to 24 bits would turn negative samples into large
    // positive values for a receiver reading the slot as int32.
    return static_cast<uint32_t>(Float32ToSigned24(sample));
}

uint32_t PcmSlotCodec::EncodeRawSigned24In32LE(float sample) noexcept {
    return ByteSwap32(EncodeRawSigned24In32BE(sample));
}

uint32_t PcmSlotCodec::EncodeFloat32(float sample, PcmSlotEncoding encoding) noexcept {
    switch (encoding) {
    case PcmSlotEncoding::RawSigned24In32BE:
        return EncodeRawSigned24In32BE(sample);
    case PcmSlotEncoding::RawSigned24In32LE:
        return EncodeRawSigned24In32LE(sample);
    case PcmSlotEncoding::Am824MBLA:
        break;
    }
    return EncodeAm824MBLA(sample);
}

void PcmSlotCodec::EncodeInterleavedFloat32Frame(const float* sourceFrame,
                                                 uint32_t sourceChannels,
                                                 uint32_t* destinationSlots,
                                                 uint32_t destinationSlotCount,
                                                 uint32_t pcmChannels,
                                                 PcmSlotEncoding encoding) noexcept {
    if (sourceFrame == nullptr || destinationSlots == nullptr) {
        return;
    }
    const uint32_t slotsToFill =
        (pcmChannels < destinationSlotCount) ? pcmChannels : destinationSlotCount;
    for (uint32_t slot = 0; slot < slotsToFill; ++slot) {
        const float sample = (slot < sourceChannels) ? sourceFrame[slot] : 0.0f;
        destinationSlots[slot] = EncodeFloat32(sample, encoding);
    }
    // Non-PCM slots (MIDI etc.) are owned by the payload writer's policy, not
    // the codec; they are left untouched here.
}

} // namespace ASFW::Protocols::Audio::AMDTP
