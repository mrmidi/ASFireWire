// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <span>

// Payload-opaque native transport adapter. Default-endian NuDCL receive with
// headerBytes=8 only. See NATIVE_RECEIVE.md for Apple binary/source evidence.
namespace NativePackets {
enum class Error { none, status, capacity, header, channel, timestamp };
struct Received {
    std::span<const uint8_t> payload{};
    uint32_t cycleStamp = 0;
    uint8_t tag = 0, sync = 0;
    Error error = Error::none;
    explicit operator bool() const { return error == Error::none; }
};
constexpr uint32_t little32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
// The caller must establish completed DMA + update-before-callback, keep the
// mapping alive and unreused during consumption, and pass the current generation
// to the content owner. Allocation capacity is not a received payload length.
inline Received receive(std::span<const uint8_t> buffer, uint32_t status,
                        uint32_t timestamp, unsigned expectedChannel) {
    if ((status & 0x1f) != 0x11) return {{},0,0,0,Error::status};
    if (buffer.size() < 8) return {{},0,0,0,Error::capacity};
    // Raw DMA prefix: timestamp quadlet, then little-endian iso header. Content
    // bytes beginning at +8 retain bus byte order; do not swap the whole buffer.
    const auto header = little32(buffer.data()+4);
    if (((header >> 4) & 15) != 0xa) return {{},0,0,0,Error::header};
    if (expectedChannel >= 63 || ((header >> 8) & 63) != expectedChannel)
        return {{},0,0,0,Error::channel};
    const auto length = header >> 16;
    if (length > buffer.size()-8) return {{},0,0,0,Error::capacity};
    // SetDCLTimeStampPtr produces low sixteen OHCI timestamp bits shifted by
    // twelve. The Apple user-DCL variant can correct a controller second bit;
    // use its updated value, not an independently reconstructed prefix stamp.
    if ((timestamp & 0xf0000fff) || ((timestamp >> 12) & 0x1fff) >= 8000)
        return {{},0,0,0,Error::timestamp};
    const auto stamp = ((timestamp >> 25) & 7)*8000 + ((timestamp >> 12) & 0x1fff);
    return {buffer.subspan(8,length),stamp,uint8_t((header>>14)&3),uint8_t(header&15),Error::none};
}
} // namespace NativePackets
