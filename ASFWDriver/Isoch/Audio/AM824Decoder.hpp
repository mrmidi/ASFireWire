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
    /// Extract 24-bit PCM from AM824 quadlet
    /// @param quadlet_be Raw quadlet in Big Endian (bus order)
    /// @return 24-bit PCM (sign-extended to int32), or nullopt if not Value data
    [[nodiscard]] static std::optional<int32_t> DecodeSample(uint32_t quadlet_be) noexcept {
        uint32_t q = SwapBigToHost(quadlet_be);
        
        // Label is top 8 bits
        uint8_t label = (q >> 24) & 0xFF;
        
        // IEC 61883-6 Table 1 - Label Codes
        // 0x40 - 0x4F: Multi-bit Linear Audio (MBLA)
        // 0x60 - 0x6F: MBLA (Legacy/Allowed?) - Wait, spec says 0x40.
        // Some devices use 0x40. 
        // 0x4n where n is channel number modulo something? No, label is fixed 0x40 usually.
        
        if (label == 0x40) {
            // 24-bit PCM: bits 0-23
            // Sign extend 24-bit to 32-bit
            int32_t sample = static_cast<int32_t>(q & 0x00FFFFFF);
            if (sample & 0x800000) {
                sample |= 0xFF000000;
            }
            return sample;
        }
        
        return std::nullopt;
    }
    
    /// Check if quadlet is MIDI
    [[nodiscard]] static bool IsMIDI(uint32_t quadlet_be) noexcept {
        uint32_t q = SwapBigToHost(quadlet_be);
        uint8_t label = (q >> 24) & 0xFF;
        // 0x80 - 0x82: MIDI conformant data
        return (label >= 0x80 && label <= 0x83);
    }
};

} // namespace ASFW::Isoch
