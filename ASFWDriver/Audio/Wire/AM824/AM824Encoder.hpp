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

#include <DriverKit/IOLib.h>

#include <cstdint>

namespace ASFW {
namespace Encoding {

/// Normalize a 24-bit signed PCM value stored in the low 24 bits of a 32-bit word.
///
/// Some producers provide 24-in-32 samples without sign-extending bit 23 into the
/// upper byte. The Saffire raw 9-slot playback path expects canonical signed 32-bit
/// values before byte-swapping to wire order, so this helper reconstructs that form.
constexpr int32_t NormalizeSigned24In32LowAligned(int32_t pcmSample) noexcept {
    uint32_t sample24 = static_cast<uint32_t>(pcmSample) & 0x00FFFFFFu;
    if ((sample24 & 0x00800000u) != 0u) {
        sample24 |= 0xFF000000u;
    }
    return static_cast<int32_t>(sample24);
}

/// AM824 label byte for MBLA (Multi-bit Linear Audio)
constexpr uint8_t kAM824LabelMBLA = 0x40;
/// AM824 label base for MIDI conformant data (IEC 61883-6)
constexpr uint8_t kAM824LabelMIDIConformantBase = 0x80;

/// Encodes 24-bit PCM audio samples to AM824 format.
/// AM824 quadlet layout (big-endian on wire):
///   Byte 0: Label (0x40 for MBLA)
///   Byte 1-3: 24-bit audio sample (MSB first)
/// Code-generation observation (Apple clang 21.0.0, arm64, -O3): a standalone
/// expression equivalent to `encode()` used `__builtin_bswap32` after applying
/// the 24-bit mask and MBLA label. Its scalar assembly was:
///     mov   w8, #1073741824       // 0x40000000, MBLA label
///     bfxil w8, w0, #0, #24       // insert the 24 sample bits
///     rev   w0, w8                // convert quadlet to wire byte order
///     ret
/// A separate counted loop over uint32_t input/output arrays produced this
/// representative vector body, followed by a scalar `rev` tail:
///     ldp         q0, q1, [x10, #-32]
///     bic.4s      v0, #255, lsl #24
///     bic.4s      v1, #255, lsl #24
///     orr.4s      v0, #64, lsl #24
///     orr.4s      v1, #64, lsl #24
///     rev32.16b   v0, v0
///     rev32.16b   v1, v1
///     stp         q0, q1, [x11, #-32]
/// That loop is an isolated code-generation experiment, not a path in this
/// type: `encode()` handles one sample and `encodeStereoFrame()` handles two.
/// The compiler also emitted a pointer-separation check before the vector
/// loop. Inlining, aliasing, loop shape, and build flags can change the result;
/// these instructions establish neither a cycle cost nor a speedup in the
/// actual packet writer.
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
        return OSSwapHostToBigInt32(quadlet);
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
        return OSSwapHostToBigInt32(static_cast<uint32_t>(kAM824LabelMBLA) << 24);
    }

    /// Encode an AM824 quadlet with only a label byte and zero payload.
    /// Useful for placeholder/non-audio slots (e.g. empty MIDI conformant data).
    static constexpr uint32_t encodeLabelOnly(uint8_t label) noexcept {
        return OSSwapHostToBigInt32(static_cast<uint32_t>(label) << 24);
    }
};

} // namespace Encoding
} // namespace ASFW
