#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ConfigROMTypes.hpp"

namespace ASFW::Driver {

// Responsible for producing the 1 KB big-endian Config ROM image required by
// OHCI §7.2. ControllerCore programs the resulting buffer via HardwareInterface.
class ConfigROMBuilder {
public:
    static constexpr size_t kConfigROMSize = 1024;
    static constexpr size_t kMaxQuadlets = kConfigROMSize / sizeof(uint32_t);

    ConfigROMBuilder();

    // Legacy single-shot builder (kept for now).
    void Build(uint32_t busOptions,
               uint64_t guid,
               uint32_t nodeCapabilities,
               std::string_view vendorName);

    // New staged API: Begin -> Add* -> Finalize.
    void Begin(uint32_t busOptions, uint64_t guid, uint32_t nodeCapabilities);
    bool AddImmediateEntry(uint8_t key, uint32_t value24);
    LeafHandle AddTextLeaf(uint8_t key, std::string_view text);
    void Finalize();

    void UpdateGeneration(uint8_t generation);

    // Returns Config ROM in big-endian format (for wire transmission)
    std::span<const uint32_t> ImageBE() const;

    // Returns Config ROM in native/host byte order (for DMA buffer storage)
    // Hardware reads from host memory during bus reset and expects native endianness
    std::span<const uint32_t> ImageNative() const;

    size_t QuadletCount() const { return quadCount_; }
    uint32_t HeaderQuad() const;
    uint32_t BusInfoQuad() const;
    uint32_t GuidHiQuad() const;
    uint32_t GuidLoQuad() const;

private:
    void Reset();
    void Append(uint32_t value);
    uint16_t ComputeCRC(size_t start, size_t count) const;
    static uint16_t CRCStep(uint16_t crc, uint16_t data);
    static uint32_t MakeDirectoryEntry(uint8_t key, uint8_t type, uint32_t value);
    static constexpr uint32_t Swap32(uint32_t value) noexcept {
#if defined(__clang__) || defined(__GNUC__)
        return __builtin_bswap32(value);
#else
        return (value >> 24) |
               ((value >> 8) & 0x0000FF00u) |
               ((value << 8) & 0x00FF0000u) |
               (value << 24);
#endif
    }

    static constexpr uint32_t ToBig(uint32_t value) noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            return Swap32(value);
        }
        return value;
    }

    static constexpr uint32_t FromBig(uint32_t value) noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            return Swap32(value);
        }
        return value;
    }

    void FinaliseBIB();
    void FinaliseRootDirectory();
    LeafHandle WriteTextLeaf(std::string_view text);
    bool EnsureRootDirectory();

    std::array<uint32_t, kMaxQuadlets> words_{};      // host-endian logical image
    mutable std::array<uint32_t, kMaxQuadlets> beImage_{}; // scratch for BE view
    size_t quadCount_{0};
    size_t rootDirHeaderIndex_{static_cast<size_t>(-1)}; // sentinel until root dir started
    uint32_t lastBusOptions_{0};
    bool begun_{false};
    bool finalized_{false};
};

} // namespace ASFW::Driver
