#pragma once

#include <cstdint>

namespace ASFW::CMP::PCRBits {

// IEC 61883-1 plug control register fields. Cross-validated with
// libiec61883/src/iec61883-private.h:345-403. This is the single field-layout
// definition used by both CMPClient and the higher-level PCRSpace.
constexpr uint32_t kOnlineMask = 0x80000000u;
constexpr uint32_t kBroadcastMask = 0x40000000u;
constexpr uint32_t kBcastMask = kBroadcastMask;
constexpr uint32_t kP2PMask = 0x3F000000u;
constexpr uint8_t kP2PShift = 24;
constexpr uint32_t kReservedMask = 0x00C00000u;
constexpr uint32_t kChannelMask = 0x003F0000u;
constexpr uint8_t kChannelShift = 16;
constexpr uint32_t kDataRateMask = 0x0000C000u;
constexpr uint8_t kDataRateShift = 14;
constexpr uint32_t kOverheadMask = 0x00003C00u;
constexpr uint8_t kOverheadShift = 10;
constexpr uint32_t kOverheadIdMask = kOverheadMask;
constexpr uint8_t kOverheadIdShift = kOverheadShift;
constexpr uint32_t kPayloadMask = 0x000003FFu;

[[nodiscard]] constexpr bool IsOnline(uint32_t pcr) noexcept {
    return (pcr & kOnlineMask) != 0;
}

[[nodiscard]] constexpr bool IsBroadcast(uint32_t pcr) noexcept {
    return (pcr & kBroadcastMask) != 0;
}

[[nodiscard]] constexpr uint8_t GetP2P(uint32_t pcr) noexcept {
    return static_cast<uint8_t>((pcr & kP2PMask) >> kP2PShift);
}

[[nodiscard]] constexpr bool IsInUse(uint32_t pcr) noexcept {
    return IsBroadcast(pcr) || GetP2P(pcr) != 0;
}

[[nodiscard]] constexpr uint8_t GetChannel(uint32_t pcr) noexcept {
    return static_cast<uint8_t>((pcr & kChannelMask) >> kChannelShift);
}

[[nodiscard]] constexpr uint8_t GetDataRate(uint32_t pcr) noexcept {
    return static_cast<uint8_t>((pcr & kDataRateMask) >> kDataRateShift);
}

[[nodiscard]] constexpr uint8_t GetOverhead(uint32_t pcr) noexcept {
    return static_cast<uint8_t>((pcr & kOverheadMask) >> kOverheadShift);
}

[[nodiscard]] constexpr uint16_t GetPayload(uint32_t pcr) noexcept {
    return static_cast<uint16_t>(pcr & kPayloadMask);
}

[[nodiscard]] constexpr uint32_t SetP2P(uint32_t pcr,
                                        uint8_t p2p) noexcept {
    return (pcr & ~kP2PMask) |
           ((static_cast<uint32_t>(p2p) & 0x3Fu) << kP2PShift);
}

[[nodiscard]] constexpr uint32_t SetChannel(uint32_t pcr,
                                            uint8_t channel) noexcept {
    return (pcr & ~kChannelMask) |
           ((static_cast<uint32_t>(channel) & 0x3Fu) << kChannelShift);
}

template <typename Speed>
[[nodiscard]] constexpr uint32_t SetDataRate(uint32_t pcr,
                                             Speed speed) noexcept {
    return (pcr & ~kDataRateMask) |
           ((static_cast<uint32_t>(speed) & 0x03u) << kDataRateShift);
}

[[nodiscard]] constexpr uint32_t SetOverheadId(
    uint32_t pcr, uint8_t overheadId) noexcept {
    return (pcr & ~kOverheadMask) |
           ((static_cast<uint32_t>(overheadId) & 0x0Fu) << kOverheadShift);
}

} // namespace ASFW::CMP::PCRBits

namespace ASFW::CMP::MPRBits {

// IEC 61883-1 output master plug register fields. Cross-validated with
// libiec61883/src/iec61883-private.h:327-375.
constexpr uint32_t kDataRateMask = 0xC0000000u;
constexpr uint8_t kDataRateShift = 30;
constexpr uint32_t kBroadcastChannelMask = 0x3F000000u;
constexpr uint8_t kBroadcastChannelShift = 24;
constexpr uint32_t kPlugCountMask = 0x0000001Fu;

[[nodiscard]] constexpr uint8_t GetDataRate(uint32_t mpr) noexcept {
    return static_cast<uint8_t>((mpr & kDataRateMask) >> kDataRateShift);
}

[[nodiscard]] constexpr uint8_t GetBroadcastChannel(uint32_t mpr) noexcept {
    return static_cast<uint8_t>(
        (mpr & kBroadcastChannelMask) >> kBroadcastChannelShift);
}

[[nodiscard]] constexpr uint8_t GetPlugCount(uint32_t mpr) noexcept {
    return static_cast<uint8_t>(mpr & kPlugCountMask);
}

} // namespace ASFW::CMP::MPRBits
