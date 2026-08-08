#pragma once

#include <cstdint>
#include <optional>

#include "../../Bus/IRM/IRMClient.hpp"
#include "../AVC/CMP/PCRCodec.hpp"

namespace ASFW::Protocols::DV {

enum class ConnectionMode : uint8_t {
    Initializing = 0,
    Broadcast = 1,
    Overlay = 2,
    PointToPoint = 3,
    BroadcastFallback = 4,
};

/// Pick the lowest numbered non-broadcast channel advertised as available by
/// the IRM. CHANNELS_AVAILABLE uses bit 31 for the first channel in each word.
[[nodiscard]] constexpr std::optional<uint8_t>
FirstAvailableChannel(const IRM::ResourceSnapshot& snapshot) noexcept {
    for (uint8_t channel = 0; channel < 63; ++channel) {
        const uint32_t word = channel < 32
                                  ? snapshot.channelsAvailable31_0
                                  : snapshot.channelsAvailable63_32;
        if ((word & IRM::ChannelToBitMask(channel)) != 0) {
            return channel;
        }
    }
    return std::nullopt;
}

/// Calculate allocation units from an output PCR. This deliberately follows
/// the established IEC 61883 userspace behavior rather than estimating from a
/// nominal DV bitrate. Cross-validated with libiec61883/src/cmp.c:34-58.
[[nodiscard]] constexpr std::optional<uint32_t>
CalculateBandwidthUnits(uint32_t opcr, uint8_t effectiveSpeed) noexcept {
    if (effectiveSpeed > 2) {
        return std::nullopt;
    }

    const uint32_t payload = CMP::PCRBits::GetPayload(opcr);
    const uint32_t overhead = CMP::PCRBits::GetOverhead(opcr);
    const uint32_t base = overhead > 0 ? overhead * 32u : 512u;
    const uint32_t units =
        base + (payload + 3u) * (1u << (2u - effectiveSpeed)) * 4u;
    if (units == 0 || units > IRM::kMaxBandwidthUnitsS400) {
        return std::nullopt;
    }
    return units;
}

} // namespace ASFW::Protocols::DV
