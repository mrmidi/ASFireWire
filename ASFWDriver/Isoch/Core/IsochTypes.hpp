//
// IsochTypes.hpp
// ASFWDriver
//
// Core Isochronous Type Definitions and OHCI context definitions.
//

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <TargetConditionals.h>

namespace ASFW::Isoch {

/// Payload-opaque receive view exposed by the OHCI transport layer.
///
/// `dmaBytes` preserves the controller-produced buffer for diagnostics. `payload`
/// starts after the OHCI timestamp and IEEE-1394 isochronous header quadlets. No
/// CIP or media-format knowledge is used to construct this view.
struct IsochRxPacketView final {
    static constexpr size_t kTransportPrefixBytes = 8;

    std::span<const uint8_t> dmaBytes{};
    std::span<const uint8_t> payload{};
    uint32_t transferStatus{0};
    uint64_t callbackTimestamp{0};
    bool hasTransportPrefix{false};

    [[nodiscard]] static IsochRxPacketView FromDma(
        std::span<const uint8_t> bytes,
        uint32_t status,
        uint64_t timestamp) noexcept {
        IsochRxPacketView view{
            .dmaBytes = bytes,
            .payload = {},
            .transferStatus = status,
            .callbackTimestamp = timestamp,
            .hasTransportPrefix = bytes.size() >= kTransportPrefixBytes,
        };
        if (view.hasTransportPrefix) {
            view.payload = bytes.subspan(kTransportPrefixBytes);
        }
        return view;
    }
};

using IsochReceiveCallback =
    std::function<void(const IsochRxPacketView& packet)>;

} // namespace ASFW::Isoch
