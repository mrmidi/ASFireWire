#pragma once

#include "../Protocols/Audio/AMDTP/AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Lab {

struct PacketWordView final {
    const uint32_t* words{nullptr};
    uint32_t wordCount{0};
};

class PacketDump final {
public:
    static PacketWordView Words(const uint8_t* packetBytes,
                                uint32_t packetByteCount) noexcept;

    static uint32_t ReadWordBE(const uint8_t* packetBytes,
                               uint32_t wordIndex) noexcept;
};

} // namespace ASFW::Lab