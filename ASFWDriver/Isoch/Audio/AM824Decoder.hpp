//
// AM824Decoder.hpp
// ASFWDriver
//
// IEC 61883-6 AM824 Audio Decoder Helpers
//

#pragma once

#include "../Core/IsochTypes.hpp"

namespace ASFW::Isoch {

class AM824Decoder {
public:
    [[nodiscard]] static uint8_t Label(uint32_t quadlet_be) noexcept {
        const uint32_t q = SwapBigToHost(quadlet_be);
        return static_cast<uint8_t>((q >> 24) & 0xFF);
    }

    [[nodiscard]] static bool IsMultiBitLinearAudioLabel(uint8_t label) noexcept {
        // IEC 61883-6 AM824 MBLA labels occupy 0x40..0x4f. Linux's AM824
        // capture path trusts configured PCM positions and copies the 24-bit
        // slot payload; accepting the full MBLA label range avoids converting
        // valid PCM slots to silence when a device uses a non-zero subtype.
        return (label >= 0x40 && label <= 0x4F);
    }

    [[nodiscard]] static bool IsMidiLabel(uint8_t label) noexcept {
        return (label >= 0x80 && label <= 0x83);
    }

    [[nodiscard]] static int32_t DecodePcmPayload(uint32_t quadlet_be) noexcept {
        const uint32_t q = SwapBigToHost(quadlet_be);
        int32_t sample = static_cast<int32_t>(q & 0x00FFFFFF);
        if (sample & 0x800000) {
            sample |= 0xFF000000;
        }
        return sample;
    }

    /// Extract 24-bit PCM from AM824 quadlet
    /// @param quadlet_be Raw quadlet in Big Endian (bus order)
    /// @return 24-bit PCM (sign-extended to int32), or nullopt if not Value data
    [[nodiscard]] static std::optional<int32_t> DecodeSample(uint32_t quadlet_be) noexcept {
        const uint8_t label = Label(quadlet_be);
        if (IsMultiBitLinearAudioLabel(label)) {
            return DecodePcmPayload(quadlet_be);
        }
        
        return std::nullopt;
    }

    /// Decode a slot that stream-format discovery has already classified as PCM.
    /// Some DICE devices do not keep a stable AM824 MBLA label byte on every
    /// received PCM slot; Linux's AMDTP path trusts configured PCM positions and
    /// copies the 24-bit payload. MIDI-labelled slots are still suppressed.
    [[nodiscard]] static std::optional<int32_t> DecodeConfiguredPcmSlot(uint32_t quadlet_be) noexcept {
        const uint8_t label = Label(quadlet_be);
        if (IsMidiLabel(label)) {
            return std::nullopt;
        }
        return DecodePcmPayload(quadlet_be);
    }
    
    /// Check if quadlet is MIDI
    [[nodiscard]] static bool IsMIDI(uint32_t quadlet_be) noexcept {
        const uint8_t label = Label(quadlet_be);
        return IsMidiLabel(label);
    }
};

} // namespace ASFW::Isoch
