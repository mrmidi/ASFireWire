// Copyright (c) 2025 ASFW Project
//
// Strongly typed helpers for building IEEE 1394 Alpha PHY packets. These hide
// the legacy mask/shift macros and guarantee that we always emit the logical
// inverse quadlet required by §5.5.3 when dispatching PHY configuration traffic.

#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace ASFW::Driver {

using Quadlet = std::uint32_t;

// Helpers for endian conversion between host order and bus (big-endian) order.
constexpr Quadlet ToBusOrder(Quadlet value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(value);
    }
    return value;
}

constexpr Quadlet FromBusOrder(Quadlet value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(value);
    }
    return value;
}

struct AlphaPhyConfig {
    std::uint8_t rootId{0};            // Bits[29:24]
    bool forceRoot{false};             // Bit[23]
    bool gapCountOptimization{false};  // Bit[22] ("T" bit)
    std::uint8_t gapCount{0x3F};       // Bits[21:16], ignored if T==0

    // Bit layout helpers (host-order masks/shifts).
    static constexpr Quadlet kPacketIdentifierMask   = 0xC000'0000u;
    static constexpr unsigned kPacketIdentifierShift = 30;
    static constexpr Quadlet kRootIdMask             = 0x3F00'0000u;
    static constexpr unsigned kRootIdShift           = 24;
    static constexpr Quadlet kForceRootMask          = 0x0080'0000u;
    static constexpr unsigned kForceRootShift        = 23;
    static constexpr Quadlet kGapOptMask             = 0x0040'0000u;
    static constexpr unsigned kGapOptShift           = 22;
    static constexpr Quadlet kGapCountMask           = 0x003F'0000u;
    static constexpr unsigned kGapCountShift         = 16;

    static constexpr bool IsConfigQuadletHostOrder(Quadlet quad) noexcept {
        return ((quad & kPacketIdentifierMask) >> kPacketIdentifierShift) == 0;
    }

    [[nodiscard]] constexpr Quadlet EncodeHostOrder() const noexcept {
        Quadlet quad = 0;
        quad |= (static_cast<Quadlet>(rootId & 0x3Fu) << kRootIdShift);
        if (forceRoot) {
            quad |= (1u << kForceRootShift);
        }
        if (gapCountOptimization) {
            quad |= (1u << kGapOptShift);
            quad |= (static_cast<Quadlet>(gapCount & 0x3Fu) << kGapCountShift);
        } else {
            // CRITICAL FIX: When T=0 (no gap update), encode gap=0x3F to prevent
            // buggy PHY implementations from latching gap=0. Per IEEE 1394a, the
            // gap field is "ignored" when T=0, but many real PHYs (especially
            // TSB41BA3) still latch the value from bits[21:16] regardless.
            // This bug caused bus reset storms in FireBug testing.
            quad |= (0x3Fu << kGapCountShift);
        }
        return quad;
    }

    static constexpr AlphaPhyConfig DecodeHostOrder(Quadlet quad) noexcept {
        AlphaPhyConfig cfg{};
        cfg.rootId = static_cast<std::uint8_t>((quad & kRootIdMask) >> kRootIdShift);
        cfg.forceRoot = (quad & kForceRootMask) != 0;
        cfg.gapCountOptimization = (quad & kGapOptMask) != 0;
        cfg.gapCount = static_cast<std::uint8_t>((quad & kGapCountMask) >> kGapCountShift);
        return cfg;
    }

    [[nodiscard]] constexpr bool IsExtendedConfig() const noexcept {
        return !forceRoot && !gapCountOptimization;
    }
};

struct AlphaPhyConfigPacket {
    AlphaPhyConfig header{};

    [[nodiscard]] constexpr std::array<Quadlet, 2> EncodeHostOrder() const noexcept {
        const Quadlet first = header.EncodeHostOrder();
        return {first, ~first};
    }

    [[nodiscard]] static constexpr AlphaPhyConfigPacket
    DecodeHostOrder(std::array<Quadlet, 2> quadlets) noexcept {
        AlphaPhyConfigPacket packet{};
        packet.header = AlphaPhyConfig::DecodeHostOrder(quadlets[0]);
        return packet;
    }

    [[nodiscard]] constexpr std::array<Quadlet, 2> EncodeBusOrder() const noexcept {
        auto host = EncodeHostOrder();
        return {ToBusOrder(host[0]), ToBusOrder(host[1])};
    }
};

// PHY Global Resume packets reuse the same identifier but set both R and T to
// zero, which the spec interprets as an extended packet. Apple sends 0x003c0000
// OR'd with the local PHY ID in bits[29:24], so mirror that pattern here.
struct PhyGlobalResumePacket {
    std::uint8_t phyId{0};

    [[nodiscard]] constexpr std::array<Quadlet, 2> EncodeHostOrder() const noexcept {
        const Quadlet first =
            (static_cast<Quadlet>(phyId & 0x3Fu) << AlphaPhyConfig::kRootIdShift) |
            0x003C'0000u;
        return {first, ~first};
    }

    [[nodiscard]] constexpr std::array<Quadlet, 2> EncodeBusOrder() const noexcept {
        auto host = EncodeHostOrder();
        return {ToBusOrder(host[0]), ToBusOrder(host[1])};
    }
};

} // namespace ASFW::Driver

