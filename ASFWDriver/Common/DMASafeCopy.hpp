#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::Common {

// FireWire async payloads are quadlet-aligned on the wire, but receive buffers can still
// land at addresses that are 4-byte aligned while not being 8-byte aligned. Copy from the
// source using only quadlet and byte reads so callers do not depend on wider aligned loads
// from DMA-backed memory.
inline void CopyFromQuadletAlignedDeviceMemory(std::span<uint8_t> destination,
                                               const uint8_t* source) noexcept {
    if (destination.empty() || source == nullptr) {
        return;
    }

    size_t offset = 0;
    for (; offset + sizeof(uint32_t) <= destination.size(); offset += sizeof(uint32_t)) {
        uint32_t quadlet = 0;
        __builtin_memcpy(&quadlet, source + offset, sizeof(quadlet));
        __builtin_memcpy(destination.data() + offset, &quadlet, sizeof(quadlet));
    }

    for (; offset < destination.size(); ++offset) {
        destination[offset] = source[offset];
    }
}

} // namespace ASFW::Common
