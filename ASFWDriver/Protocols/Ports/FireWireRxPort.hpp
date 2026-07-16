#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::Protocols::Ports {

struct BlockWriteRequestView {
    uint16_t sourceID{0};
    uint64_t destOffset{0};
    // Capture the receive dispatch epoch with the packet.  Consumers must not
    // substitute a later live bus generation when matching a response.
    std::optional<uint32_t> generation{};
    std::span<const uint8_t> payload{};
};

enum class BlockWriteDisposition : uint8_t {
    kComplete,
    kAddressError,
};

} // namespace ASFW::Protocols::Ports
