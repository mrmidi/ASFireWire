#pragma once
#include <cstdint>
#include <cstring>

namespace ASFW::Async {

// PODs used by GetStatusSnapshot(); keep layout stable for tooling.
struct AsyncDescriptorStatus {
    uint64_t descriptorVirt{};
    uint64_t descriptorIOVA{};
    uint32_t descriptorCount{};
    uint32_t descriptorStride{};
    uint32_t commandPtr{};
    uint32_t _pad0{};
};

struct AsyncBufferStatus {
    uint64_t bufferVirt{};
    uint64_t bufferIOVA{};
    uint32_t bufferCount{};
    uint32_t bufferSize{};
};

struct AsyncStatusSnapshot {
    uint64_t dmaSlabVirt{};
    uint64_t dmaSlabIOVA{};
    uint32_t dmaSlabSize{};
    uint32_t _pad0{};
    AsyncDescriptorStatus atRequest{};
    AsyncDescriptorStatus atResponse{};
    AsyncDescriptorStatus arRequest{};
    AsyncDescriptorStatus arResponse{};
    AsyncBufferStatus     arRequestBuffers{};
    AsyncBufferStatus     arResponseBuffers{};
};

// Simple CRC32 (IEEE 802.3) for snapshot integrity; tiny and header-only.
namespace detail {
    inline uint32_t crc32_table_entry(uint32_t i) {
        uint32_t c = i;
        for (int k=0; k<8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        return c;
    }
    inline const uint32_t* crc32_table() {
        static uint32_t tbl[256]{};
        static bool init=false;
        if (!init) { for (uint32_t i=0;i<256;++i) tbl[i]=crc32_table_entry(i); init=true; }
        return tbl;
    }
}
inline uint32_t CRC32(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    const uint32_t* T = detail::crc32_table();
    for (size_t i=0;i<n;++i) c = T[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
inline uint32_t CRC32(const AsyncStatusSnapshot& s) {
    return CRC32(&s, sizeof(AsyncStatusSnapshot));
}

} // namespace ASFW::Async
