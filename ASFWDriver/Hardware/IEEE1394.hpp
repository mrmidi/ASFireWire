#pragma once

#include <cstdint>
#include "OHCIConstants.hpp"

namespace ASFW::Async::HW {

[[nodiscard]] constexpr uint16_t ToBigEndian16(uint16_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(value);
#else
    return value;
#endif
}

[[nodiscard]] constexpr uint32_t ToBigEndian32(uint32_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

[[nodiscard]] inline constexpr uint32_t BuildIEEE1394Quadlet0(uint16_t destID, uint8_t tLabel, uint8_t retry, uint8_t tCode, uint8_t priority) noexcept {
    return (static_cast<uint32_t>(destID & 0xFFFF) << Driver::kIEEE1394_DestinationIDShift) |
           (static_cast<uint32_t>(tLabel & 0x3F) << Driver::kIEEE1394_TLabelShift) |
           (static_cast<uint32_t>(retry & 0x03) << Driver::kIEEE1394_RetryShift) |
           (static_cast<uint32_t>(tCode & 0x0F) << Driver::kIEEE1394_TCodeShift) |
           (static_cast<uint32_t>(priority & 0x0F) << Driver::kIEEE1394_PriorityShift);
}

[[nodiscard]] inline constexpr uint32_t BuildIEEE1394Quadlet1(uint16_t sourceID, uint16_t offsetHigh) noexcept {
    return (static_cast<uint32_t>(sourceID & 0xFFFF) << Driver::kIEEE1394_SourceIDShift) |
           (static_cast<uint32_t>(offsetHigh & 0xFFFF) << Driver::kIEEE1394_OffsetHighShift);
}

[[nodiscard]] inline constexpr uint32_t BuildIEEE1394Quadlet3Block(uint16_t dataLength, uint16_t extendedTCode = 0) noexcept {
    return (static_cast<uint32_t>(dataLength & 0xFFFF) << Driver::kIEEE1394_DataLengthShift) |
           (static_cast<uint32_t>(extendedTCode & 0xFFFF) << Driver::kIEEE1394_ExtendedTCodeShift);
}

struct AsyncRequestHeader {
    uint32_t control{0};
    uint16_t destinationID{0};
    uint16_t destinationOffsetHigh{0};
    uint32_t destinationOffsetLow{0};
    union {
        uint32_t quadletData;
        uint16_t dataLength;
        uint16_t extendedTCode;
    } payload_info{};
    static constexpr uint32_t kLabelShift = 10;
    static constexpr uint32_t kRetryShift = 8;
    static constexpr uint32_t kTcodeShift = 4;
    static constexpr uint8_t kTcodeWriteQuad = Driver::kIEEE1394_TCodeWriteQuadRequest;
    static constexpr uint8_t kTcodeWriteBlock = Driver::kIEEE1394_TCodeWriteBlockRequest;
    static constexpr uint8_t kTcodeReadQuad = Driver::kIEEE1394_TCodeReadQuadRequest;
    static constexpr uint8_t kTcodeReadBlock = Driver::kIEEE1394_TCodeReadBlockRequest;
    static constexpr uint8_t kTcodeLockRequest = Driver::kIEEE1394_TCodeLockRequest;
    static constexpr uint8_t kTcodeStreamData = Driver::kIEEE1394_TCodeIsochronousBlock;
    static constexpr uint8_t kTcodePhyPacket = Driver::kIEEE1394_TCodePhyPacket;
};

struct __attribute__((packed)) AsyncReceiveHeader {
    uint16_t destinationID{0};
    uint8_t  tl_tcode_rt{0};
    uint8_t  headerControl{0};
    uint16_t sourceID{0};
    uint16_t destinationOffsetHigh{0};
    uint32_t destinationOffsetLow{0};
    static constexpr uint8_t kTLabelMask = 0xFC;
    static constexpr uint8_t kTLabelShift = 2;
    static constexpr uint8_t kTCodeMask = 0x0F;
    static constexpr uint8_t kRetryShift = 6;
};

struct __attribute__((packed)) ARPacketTrailer {
    uint16_t timeStamp{0};
    uint16_t xferStatus{0};
};

} // namespace ASFW::Async::HW

namespace ASFW::Driver {
// PHY register 4 address and bitmasks (per IEEE 1394 PHY register definitions)
// PHY reg 4: Bit 7 = link_on (PHY_LINK_ACTIVE), Bit 6 = contender (PHY_CONTENDER)
constexpr uint8_t kPhyReg4Address = 4;
constexpr uint8_t kPhyLinkActive = 0x80;  // Bit 7
constexpr uint8_t kPhyContender  = 0x40;  // Bit 6
// PHY gap count mask (register-level value: lower 6 bits)
constexpr uint8_t kPhyGapCountMask = 0x3Fu; // 6-bit gap count field in PHY reg1

// PHY register 5: Bit 6 enables IEEE 1394a accelerated arbitration (Enab_accel)
constexpr uint8_t kPhyReg5Address = 5;
constexpr uint8_t kPhyEnableAcceleration = 0x40;  // Bit 6
}
