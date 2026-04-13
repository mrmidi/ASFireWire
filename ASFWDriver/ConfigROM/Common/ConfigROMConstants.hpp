#pragma once

#include <cstdint>

#include "../../Discovery/DiscoveryTypes.hpp"

namespace ASFW::ConfigROM {

inline constexpr uint32_t kQuadletBytes = 4;
inline constexpr uint32_t kBIBLengthBytes = 20;
inline constexpr uint32_t kBIBQuadletCount = 5;
inline constexpr uint32_t kHeaderFirstMaxEntries = 64;
inline constexpr uint32_t kMaxROMPrefixQuadlets = 256;
inline constexpr uint32_t kMaxROMBytes = 1024;

/**
 * @brief Minimum alignment for Configuration ROM DMA buffers.
 *
 * OHCI 1.1 §5.5.6 specifies that the system address for the Config ROM must start
 * on a 1 KB boundary, as the low order 10 bits of the mapping register are reserved
 * and assumed to be zero.
 */
inline constexpr uint64_t kRomAlignmentBytes = 1024;

[[nodiscard]] constexpr uint32_t
RootDirStartQuadlet(const ASFW::Discovery::BusInfoBlock& bib) noexcept {
    return 1U + static_cast<uint32_t>(bib.busInfoLength);
}

[[nodiscard]] constexpr uint32_t
RootDirStartBytes(const ASFW::Discovery::BusInfoBlock& bib) noexcept {
    return RootDirStartQuadlet(bib) * kQuadletBytes;
}

} // namespace ASFW::ConfigROM
