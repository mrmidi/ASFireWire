//
// CIPHeader.hpp
// ASFWDriver
//
// IEC 61883-1 Common Isochronous Packet Header
//

#pragma once

#include <DriverKit/IOLib.h>

#include <cstdint>
#include <array>
#include <optional>
#include "../../Isoch/Core/IsochTypes.hpp"

namespace ASFW::Isoch {

struct CIPHeader {
    // Source node ID (filled by hardware on TX, parsed on RX)
    uint8_t sourceNodeId{0};

    // Data Block Size (quadlets per data block)
    uint8_t dataBlockSize{0};

    // Source Packet Header flag
    bool sourcePacketHeader{false};

    // Data Block Counter (0-255, wraps)
    uint8_t dataBlockCounter{0};

    // Format code (0x00 = DVCR, 0x10 = AM824)
    uint8_t format{0x10};  // AM824 for audio

    // Format Dependent Field (sample rate for AM824)
    uint8_t fdf{0};

    // Synchronization timestamp (0xFFFF = no info)
    uint16_t syt{0xFFFF};

    /// Decode from two quadlets (bus order)
    [[nodiscard]] static std::optional<CIPHeader> Decode(uint32_t q0_be, uint32_t q1_be) noexcept {
        uint32_t q0 = OSSwapBigToHostInt32(q0_be);
        uint32_t q1 = OSSwapBigToHostInt32(q1_be);

        // Quadlet 0: [EOH:1=0][SID:6][DBS:8][FN:2][QPC:3][SPH:1][rsv:3][DBC:8]
        // Reference: IEC 61883-1
        
        uint8_t eoh0 = (q0 >> 31) & 0x1;
        if (eoh0 != 0) return std::nullopt; // First quadlet EOH must be 0

        CIPHeader h;
        h.sourceNodeId = (q0 >> 24) & 0x3F;
        h.dataBlockSize = (q0 >> 16) & 0xFF;
        h.sourcePacketHeader = (q0 >> 10) & 0x1;
        h.dataBlockCounter = q0 & 0xFF;
        
        // Quadlet 1: [EOH:1=1][rsv:1][FMT:6][FDF:8][SYT:16]
        // FMT = 0x10 for AM824
        
        uint8_t eoh1 = (q1 >> 31) & 0x1;
        if (eoh1 != 1) return std::nullopt; // Second quadlet EOH must be 1

        h.format = (q1 >> 24) & 0x3F;
        h.fdf = (q1 >> 16) & 0xFF;
        h.syt = q1 & 0xFFFF;
        
        return h;
    }
};

} // namespace ASFW::Isoch
