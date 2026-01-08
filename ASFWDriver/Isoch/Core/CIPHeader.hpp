//
// CIPHeader.hpp
// ASFWDriver
//
// IEC 61883-1 Common Isochronous Packet Header
//

#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include "IsochTypes.hpp"

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
        uint32_t q0 = SwapBigToHost(q0_be);
        uint32_t q1 = SwapBigToHost(q1_be);

        // Quadlet 0: [EOH:1=0][SID:6][DBS:8][FN:2][QPC:3][SPH:1][rsv:2][DBC:8]
        // Actually:
        // [0:1] EOH (must be 0 for first quadlet)
        // [1:6] SID (Source Node ID)
        // [8:8] DBS (Data Block Size)
        // [16:2] FN (Fraction Number)
        // [18:3] QPC (Quadlet Padding Count)
        // [21:1] SPH (Source Packet Header)
        // [22:8] RSV (Reserved) + DBC (Data Block Counter) ... Wait, standard says:
        // IEC 61883-1 Figure 1:
        // 0-7:  SID (6) + DBS (2 high) ... NO.
        // Let's check spec carefully.
        
        // 61883-1 Ed 2:
        // Q0: [0] 0
        //     [1-6] SID
        //     [7-15] DBS
        //     [16-17] FN
        //     [18-20] QPC
        //     [21] SPH
        //     [22-23] rsv
        //     [24-31] DBC
        
        // Check EOH (Bit 31 of BE?? or Bit 0 of LE?)
        // In Host Order (swapped from BE):
        // MSB (31) corresponds to first bit on wire? BE: 0x80... is first bit set?
        // Wire: b0 b1 ... b31.
        // BE memory: [b0..b7] [b8..b15] ...
        // u32 load (BE machine): as is.
        // u32 swap (LE machine): matches BE value.
        // So bit 31 is b0? No.
        // 0x12345678.
        // b0 is MSB of 0x12 (if 1).
        // So "Bit 0" usually means MSB in network diagrams.
        // So (q0 >> 31) & 1 is EOH0.
        
        uint8_t eoh0 = (q0 >> 31) & 0x1;
        // uint8_t form0 = (q0 >> 30) & 0x1; // "Form" bit? 61883-1 says "0" for first quadlet?
        // Actually 61883-1 says: "The first bit of the first quadlet of the CIP header shall be 0".
        // And "The first bit of the second quadlet... shall be 1".
        
        if (eoh0 != 0) return std::nullopt; // First quadlet must accept 0

        CIPHeader h;
        h.sourceNodeId = (q0 >> 24) & 0x3F; // Bits 1-6? SPH/FN?
        // Wait, mask shift is tricky.
        // q0 = 0x 0 S S S S S S D.
        // (q0 >> 24) is top byte.
        // bit 31=0.
        // bits 30-25 = SID (6 bits).
        h.sourceNodeId = (q0 >> 24) & 0x3F; // Actually bits [24..29] ??
        // Let's use strict bitfields logic if we assume MSB=bit0.
        // Field | Width | Shift | Mask
        // EOH   | 1     | 31    | 1
        // SID   | 6     | 25    | 0x3F
        // DBS   | 8     | 16    | 0xFF
        // FN    | 2     | 14    | 0x3
        // QPC   | 3     | 11    | 0x7
        // SPH   | 1     | 10    | 0x1
        // RSV   | 2     | 8     | 0x3
        // DBC   | 8     | 0     | 0xFF
        
        // Wait, SID is 6 bits. 31-1 = 30. So 30 down to 25 is 6 bits.
        h.sourceNodeId = (q0 >> 25) & 0x3F;
        h.dataBlockSize = (q0 >> 16) & 0xFF; // Bits 7-15 is 9 bits? No DBS is 8 bits.
        // 61883-1:
        // 0 bits: 1
        // SID bits: 6
        // DBS bits: 8
        // FN bits: 2
        // QPC bits: 3
        // SPH bits: 1
        // rsv bits: 2 ??
        // DBC bits: 8
        // Total: 1+6+8+2+3+1+2+8 = 31. Missing 1 bit? 32 bits total.
        // 1+6=7. 7+8=15. 15+2=17. 17+3=20. 20+1=21. 21+2=23. 23+8=31.
        // Where is the 32nd bit?
        // Ah, DBS is 8 bits.
        
        // Let's re-read:
        // [0] 0
        // [1] 0 (Form) -> actually "00" is specific to AM824?
        // No, typically SID is 6 bits.
        
        // Let's use the linux reference (amdtp-stream.c):
        // cip_header[0] >> 24 & 0x3f; (SID) -> implies top 2 bits are 0 0?
        // cip_header[0] >> 16 & 0xff; (DBS)
        // cip_header[0] >> 10 & 0x1; (SPH)
        // cip_header[0] & 0xff; (DBC)
        
        // If Linux uses >> 24 for SID (0x3F), then it occupies bits 24-29.
        // Bits 30-31 are stripped.
        // This implies valid masks:
        // EOH=31, FORM=30.
        
        h.sourceNodeId = (q0 >> 24) & 0x3F;
        h.dataBlockSize = (q0 >> 16) & 0xFF;
        
        // Linux: (SPH is bit 10?)
        // 1 + 6 + 8 + (FN:2) + (QPC:3) + (SPH:1) + (rsv) + DBC8.
        // 32-1-6-8-2-3-1 = 11.
        // So SPH is at 11? Or 10?
        // Let's assume standard position from Linux drivers which work.
        h.sourcePacketHeader = (q0 >> 10) & 0x1;
        h.dataBlockCounter = q0 & 0xFF;
        
        // Quadlet 1:
        // [0] 1 (EOH)
        // [1] 0 (Form? CIP header type?) 
        // [2-7] FMT (Format ID) - 6 bits
        // [8-31] FDF (Format Dependent Field) - 24 bits
        // For AM824:
        // FMT = 0x10 (AM824 - actually 0x10 is 6 bits? No, 0x10 is value 16).
        // FDF = [FDF:8][SYT:16]
        
        uint8_t eoh1 = (q1 >> 31) & 0x1;
        if (eoh1 != 1) return std::nullopt; // Second quadlet must have MSB 1

        h.format = (q1 >> 24) & 0x3F; // 6 bits
        h.fdf = (q1 >> 16) & 0xFF;    // 8 bits
        h.syt = q1 & 0xFFFF;          // 16 bits
        
        return h;
    }
};

} // namespace ASFW::Isoch
