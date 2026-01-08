// AM824Encoder.hpp
// ASFW - Phase 1.5 Encoding Layer
//
// Converts 24-bit PCM audio samples to AM824 quadlets per IEC 61883-6.
// AM824 format: [0x40 label][24-bit big-endian sample]
//
// Reference: docs/Isoch/PHASE_1_5_ENCODING.md
// Verified against: 000-48kORIG.txt FireBug capture
//

#pragma once

#include <cstdint>

namespace ASFW {
namespace Encoding {

/// AM824 label byte for MBLA (Multi-bit Linear Audio)
constexpr uint8_t kAM824LabelMBLA = 0x40;

/// Encodes 24-bit PCM audio samples to AM824 format.
/// AM824 quadlet layout (big-endian on wire):
///   Byte 0: Label (0x40 for MBLA)
///   Byte 1-3: 24-bit audio sample (MSB first)
struct AM824Encoder {
    
    /// Encode a single PCM sample to AM824 format.
    /// 
    /// @param pcmSample 32-bit signed integer with 24-bit audio in LOWER bits
    ///                  (standard AudioDriverKit 24-in-32 format: 0x00XXXXXX)
    /// @return AM824 quadlet in big-endian wire order
    ///
    /// Example:
    ///   Input:  0x00f3729e (24-bit sample in lower bits)
    ///   Output: 0x40f3729e (label 0x40 + sample) → byte-swapped for wire
    ///
    static constexpr uint32_t encode(int32_t pcmSample) noexcept {
        // Extract 24-bit sample from LOWER bits of 32-bit container
        // AudioDriverKit uses sign-extended 24-in-32: sample in bits [23:0]
        uint32_t sample24 = static_cast<uint32_t>(pcmSample) & 0x00FFFFFF;
        
        // Combine with AM824 label in MSB position
        uint32_t quadlet = (static_cast<uint32_t>(kAM824LabelMBLA) << 24) | sample24;
        
        // Byte swap for big-endian FireWire wire order
        // Host (little-endian): [label][hi][mid][lo]
        // Wire (big-endian):    [lo][mid][hi][label] after swap
        return byteSwap32(quadlet);
    }
    
    /// Encode a stereo frame (2 samples) to AM824 format.
    ///
    /// @param left  Left channel sample (24-in-32)
    /// @param right Right channel sample (24-in-32)
    /// @param out   Output array (must have space for 2 uint32_t)
    static constexpr void encodeStereoFrame(int32_t left, int32_t right,
                                            uint32_t* out) noexcept {
        out[0] = encode(left);
        out[1] = encode(right);
    }
    
    /// Encode silence (zero sample) to AM824 format.
    /// Returns 0x40000000 in wire order.
    static constexpr uint32_t encodeSilence() noexcept {
        return byteSwap32(static_cast<uint32_t>(kAM824LabelMBLA) << 24);
    }
    
private:
    /// Byte swap for endianness conversion.
    /// Equivalent to __builtin_bswap32 but constexpr-safe.
    static constexpr uint32_t byteSwap32(uint32_t x) noexcept {
        return ((x & 0xFF000000) >> 24) |
               ((x & 0x00FF0000) >> 8)  |
               ((x & 0x0000FF00) << 8)  |
               ((x & 0x000000FF) << 24);
    }
};

} // namespace Encoding
} // namespace ASFW
