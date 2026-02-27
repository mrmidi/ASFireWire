#pragma once

#include <cstdint>

#include "../Discovery/DiscoveryTypes.hpp"

namespace ASFW::ConfigROM {

inline constexpr uint32_t kQuadletBytes = 4;
inline constexpr uint32_t kBIBLengthBytes = 20;
inline constexpr uint32_t kBIBQuadletCount = 5;
inline constexpr uint32_t kHeaderFirstMaxEntries = 64;
inline constexpr uint32_t kMaxROMPrefixQuadlets = 256;
inline constexpr uint32_t kMaxROMBytes = 1024;
inline constexpr uint64_t kRomAlignmentBytes = 1024; // OHCI 1.1 section 5.5.6

[[nodiscard]] constexpr uint32_t RootDirStartQuadlet(const ASFW::Discovery::BusInfoBlock& bib) noexcept {
    return 1u + static_cast<uint32_t>(bib.busInfoLength);
}

[[nodiscard]] constexpr uint32_t RootDirStartBytes(const ASFW::Discovery::BusInfoBlock& bib) noexcept {
    return RootDirStartQuadlet(bib) * kQuadletBytes;
}

} // namespace ASFW::ConfigROM
