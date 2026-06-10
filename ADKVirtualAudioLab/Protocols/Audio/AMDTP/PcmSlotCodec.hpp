#pragma once

#include "AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

class PcmSlotCodec final {
public:
    [[nodiscard]] static int32_t Float32ToSigned24(float sample) noexcept;

    [[nodiscard]] static uint32_t EncodeFloat32(float sample,
                                                PcmSlotEncoding encoding) noexcept;

    static void EncodeInterleavedFloat32Frame(const float* sourceFrame,
                                              uint32_t sourceChannels,
                                              uint32_t* destinationSlots,
                                              uint32_t destinationSlotCount,
                                              uint32_t pcmChannels,
                                              PcmSlotEncoding encoding) noexcept;

private:
    [[nodiscard]] static uint32_t EncodeAm824MBLA(float sample) noexcept;
    [[nodiscard]] static uint32_t EncodeRawSigned24In32BE(float sample) noexcept;
    [[nodiscard]] static uint32_t EncodeRawSigned24In32LE(float sample) noexcept;
};

} // namespace ASFW::Protocols::Audio::AMDTP