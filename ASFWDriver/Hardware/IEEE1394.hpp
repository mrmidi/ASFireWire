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
// cross-validated with Linux: packet-header-definitions.h:12-30 packet-serdes-test.c:67-108
static_assert(BuildIEEE1394Quadlet0(0xffc0u, 0x3cu, 0x1u, Driver::kIEEE1394_TCodeReadQuadRequest, 0u) ==
              0xffc0f140u);
static_assert(BuildIEEE1394Quadlet1(0xffc1u, 0xffffu) == 0xffc1ffffu);
static_assert(BuildIEEE1394Quadlet3Block(0x0020u, 0u) == 0x00200000u);

// Constants holder only. Do not reinterpret DMA bytes as this type; packet
// builders/parsers use explicit quadlet construction so endian order is visible.
struct AsyncRequestHeader {
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
static_assert(AsyncRequestHeader::kLabelShift == Driver::kIEEE1394_TLabelShift);
static_assert(AsyncRequestHeader::kRetryShift == Driver::kIEEE1394_RetryShift);
static_assert(AsyncRequestHeader::kTcodeShift == Driver::kIEEE1394_TCodeShift);
static_assert(AsyncRequestHeader::kTcodeWriteQuad == 0x0);
static_assert(AsyncRequestHeader::kTcodeWriteBlock == 0x1);
static_assert(AsyncRequestHeader::kTcodeReadQuad == 0x4);
static_assert(AsyncRequestHeader::kTcodeReadBlock == 0x5);
static_assert(AsyncRequestHeader::kTcodeLockRequest == 0x9);
static_assert(AsyncRequestHeader::kTcodeStreamData == 0xA);
static_assert(AsyncRequestHeader::kTcodePhyPacket == 0xE);

} // namespace ASFW::Async::HW

namespace ASFW::Driver {
// PHY register 4 address and bitmasks (per IEEE 1394 PHY register definitions)
// PHY reg 4: Bit 7 = link_on (PHY_LINK_ACTIVE), Bit 6 = contender (PHY_CONTENDER)
constexpr uint8_t kPhyReg4Address = 4;
constexpr uint8_t kPhyLinkActive = 0x80;  // Bit 7
constexpr uint8_t kPhyContender  = 0x40;  // Bit 6
static_assert(kPhyLinkActive == 0x80 && kPhyContender == 0x40,
              "PHY reg4 link/contender bits must match Linux core.h");
// PHY gap count mask (register-level value: lower 6 bits)
constexpr uint8_t kPhyGapCountMask = 0x3Fu; // 6-bit gap count field in PHY reg1

// PHY register 1: bus reset control
// Bit 6 = IBR (Initiate Bus Reset) — long bus reset
constexpr uint8_t kPhyReg1Address = 1;
constexpr uint8_t kPhyRootHoldOff = 0x80;       // Bit 7
constexpr uint8_t kPhyInitiateBusReset = 0x40;  // Bit 6
static_assert(kPhyGapCountMask == 0x3F && kPhyRootHoldOff == 0x80 &&
              kPhyInitiateBusReset == 0x40,
              "PHY reg1 gap/RHB/IBR bits must match IEEE 1394 PHY register layout");

// PHY register 5: IEEE 1394a enhancement bits.
// cross-validated with Linux: core.h:39-44 ohci.c:2372-2377
// Bit 6 is SBR (Initiate Short Bus Reset); acceleration/multi are low bits.
// Per Linux firewire_ohci configure_1394a_enhancements(): both bits are set together.
constexpr uint8_t kPhyReg5Address = 5;
constexpr uint8_t kPhyEnableAcceleration = 0x02;
constexpr uint8_t kPhyInitiateShortBusReset = 0x40; // Bit 6 = SBR (IEEE 1394a)
constexpr uint8_t kPhyEnableMulti = 0x01;
static_assert(kPhyEnableAcceleration == 0x02 && kPhyEnableMulti == 0x01,
              "PHY reg5 IEEE 1394a enhancement bits must match Linux core.h");
static_assert(kPhyInitiateShortBusReset != kPhyEnableAcceleration,
              "PHY reg5 SBR must not alias accelerated arbitration");
}
