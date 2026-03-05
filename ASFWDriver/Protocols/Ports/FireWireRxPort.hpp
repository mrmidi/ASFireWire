#pragma once

#include <cstdint>
#include <span>

namespace ASFW::Protocols::Ports {

struct BlockWriteRequestView {
    uint16_t sourceID{0};
    uint64_t destOffset{0};
    std::span<const uint8_t> payload{};
};

enum class BlockWriteDisposition : uint8_t {
    kComplete,
    kAddressError,
};

} // namespace ASFW::Protocols::Ports
