#pragma once

#include "DirectRxTypes.hpp"
#include "../../../Wire/AM824/AM824Decoder.hpp"
#include "../../../Wire/RawPcm24In32/RawPcm24In32Decoder.hpp"
#include <cstdint>

namespace ASFW::AudioEngine::Direct::Rx {

inline void DecodeDirectRxFrame(const uint32_t* inWireQuadlets,
                                uint32_t pcmChannels,
                                uint32_t am824Slots,
                                ASFW::Encoding::AudioWireFormat format,
                                int32_t* outPcmFrame) noexcept {
    for (uint32_t ch = 0; ch < pcmChannels; ++ch) {
        if (format == ASFW::Encoding::AudioWireFormat::kRawPcm24In32) {
            auto sample = ASFW::Encoding::RawPcm24In32::Decode(inWireQuadlets[ch]);
            outPcmFrame[ch] = sample ? *sample : 0;
        } else {
            auto sample = ASFW::Isoch::AM824Decoder::DecodeSample(inWireQuadlets[ch]);
            outPcmFrame[ch] = sample ? *sample : 0;
        }
    }
}

} // namespace ASFW::AudioEngine::Direct::Rx
