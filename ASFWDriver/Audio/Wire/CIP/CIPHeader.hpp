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
#include "../../../Isoch/Core/IsochTypes.hpp"

namespace ASFW::Isoch {

/**
 * @struct CIPHeader
 * @brief Represents the IEC 61883-1 Common Isochronous Packet (CIP) Header.
 * 
 * This structure explicitly models the two-quadlet CIP header format 
 * defined for fixed-length payload streams, specifically targeting Audio 
 * and Music Data (AM824).
 * 
 * Reference: 
 * - IEC 61883-1 §6.2.1: Two-quadlet CIP header
 * - IEC 61883-6 §6.3: CIP Header format
 */
struct CIPHeader {
    /** 
     * Source Node ID (SID). 
     * Indicates the 6-bit physical_ID of the transmitting node.
     * Filled by OHCI hardware on TX, parsed on RX.
     * Reference: IEC 61883-1 §6.2.1
     */
    uint8_t sourceNodeId{0};

    /** 
     * Data Block Size (DBS).
     * Specifies the size of a single data block in quadlets (32-bit words).
     * Valid range: 0x01 (1 quadlet) to 0xFF (255 quadlets). 0x00 = 256 quadlets.
     * Reference: IEC 61883-1 §6.2.1
     */
    uint8_t dataBlockSize{0};

    /** 
     * Source Packet Header (SPH) flag.
     * If true (1), indicates the source packet has its own payload header.
     * Reference: IEC 61883-1 §6.2.1
     */
    bool sourcePacketHeader{false};

    /** 
     * Data Block Continuity (DBC) counter.
     * An 8-bit wrap-around counter used to detect missing or dropped data blocks.
     * Increments per data block, not per packet.
     * Reference: IEC 61883-1 §6.2.1
     */
    uint8_t dataBlockCounter{0};

    /** 
     * Format ID (FMT).
     * Specifies the high-level format of the real-time data.
     * 0x10 (10_16) explicitly designates Audio and Music Data (AM824).
     * Reference: IEC 61883-1 §6.2.1 Table 5 / IEC 61883-6 §6.3 Table 2
     */
    uint8_t format{0x10}; 

    /** 
     * Format Dependent Field (FDF).
     * For AM824 (FMT = 0x10), this field contains the Sampling Frequency Code (SFC)
     * and rate control flags.
     * Reference: IEC 61883-6 §9.1 and §10
     */
    uint8_t fdf{0};

    /** 
     * Synchronization Time-stamp (SYT).
     * Represents the presentation time of the first data block in the packet, 
     * derived from the lower 16 bits of the IEEE 1394 CYCLE_TIME register.
     * 0xFFFF indicates "No Information" (no timestamp present).
     * Reference: IEC 61883-1 §6.2.1 Table 6 / IEC 61883-6 §6.3 Table 2
     */
    uint16_t syt{0xFFFF};

    /**
     * @brief Decodes the CIP header from the first two quadlets of an isochronous payload.
     * 
     * @param q0_be First quadlet directly from the bus (Big Endian)
     * @param q1_be Second quadlet directly from the bus (Big Endian)
     * @return std::optional<CIPHeader> Valid CIP header if EOH markers match, nullopt otherwise.
     */
    [[nodiscard]] static std::optional<CIPHeader> Decode(uint32_t q0_be, uint32_t q1_be) noexcept {
        uint32_t q0 = OSSwapBigToHostInt32(q0_be);
        uint32_t q1 = OSSwapBigToHostInt32(q1_be);

        // Quadlet 0: [EOH_0:1=0][form_0:1=0][SID:6][DBS:8][FN:2][QPC:3][SPH:1][rsv:2][DBC:8]
        // Reference: IEC 61883-1 §6.2.1, Figure 5
        
        uint8_t eoh0 = (q0 >> 31) & 0x1;
        if (eoh0 != 0) return std::nullopt; // First quadlet EOH must be 0

        CIPHeader h;
        h.sourceNodeId       = (q0 >> 24) & 0x3F;
        h.dataBlockSize      = (q0 >> 16) & 0xFF;
        h.sourcePacketHeader = (q0 >> 10) & 0x1;
        h.dataBlockCounter   = q0 & 0xFF;
        
        // Quadlet 1: [EOH_1:1=1][form_1:1=0][FMT:6][FDF:8][SYT:16]
        // Reference: IEC 61883-1 §6.2.1, Figure 5
        
        uint8_t eoh1 = (q1 >> 31) & 0x1;
        if (eoh1 != 1) return std::nullopt; // Second quadlet EOH must be 1

        h.format = (q1 >> 24) & 0x3F; // Expected 0x10 for AM824 Audio/Music
        h.fdf    = (q1 >> 16) & 0xFF; // Contains Sampling Frequency Code (SFC)
        h.syt    = q1 & 0xFFFF;       // 0xFFFF = No Info
        
        return h;
    }
};

} // namespace ASFW::Isoch
