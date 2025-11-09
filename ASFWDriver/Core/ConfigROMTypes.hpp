#pragma once

#include <cstdint>
#include <string_view>

namespace ASFW::Driver {

// IEEE 1212 root directory keys (subset – extend as needed)
enum class ROMRootKey : uint8_t {
    Vendor_ID          = 0x03, // Immediate: 24-bit company_id (guid[63:40])
    Node_Capabilities  = 0x0C, // Immediate: capability flags (software policy)
    Vendor_Text        = 0x81, // Leaf: textual descriptor (ASCII)
};

// Directory entry type field (2 bits) – see IEEE 1212 §7.2 / §8.3
enum class ROMEntryType : uint8_t {
    Immediate = 0, // value field is immediate 24-bit value
    CSROffset = 1, // points to CSR address space (not yet used)
    Leaf      = 2, // offset (in quadlets) to leaf block
    Directory = 3, // offset (in quadlets) to sub-directory
};

// Handle identifying a created text leaf (allow future introspection)
struct LeafHandle {
    uint16_t offsetQuadlets = 0; // Quadlet offset from start of image to leaf header
    bool valid() const { return offsetQuadlets != 0; }
};

// Helper to build a directory entry (host-endian)
constexpr uint32_t MakeDirectoryEntry(ROMRootKey key, ROMEntryType type, uint32_t value24) {
    return (static_cast<uint32_t>(type) & 0x3u) << 30 |
           (static_cast<uint32_t>(static_cast<uint8_t>(key)) & 0x3Fu) << 24 |
           (value24 & 0x00FFFFFFu);
}

// Bus name constant '1394' (ASCII) per OHCI 1.1 §7.2
constexpr uint32_t kBusNameQuadlet = 0x31333934u; // '1394'

// CRC polynomial for IEEE 1212 (same as ITU-T CRC-16)
constexpr uint16_t kConfigROMCRCPolynomial = 0x1021;

} // namespace ASFW::Driver
