#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ConfigROMTypes.hpp"

namespace ASFW::Driver {

/**
 * @class ConfigROMBuilder
 * @brief Builds the driver's local IEEE 1212 / IEEE 1394 Configuration ROM image.
 *
 * The builder maintains a host-order logical image and can expose a big-endian
 * (wire-order) view for export/debugging. The ConfigROMStager consumes the host-order
 * image when staging the ROM to OHCI hardware.
 */
class ConfigROMBuilder {
  public:
    /** @brief Maximum size of the Configuration ROM image (1 KiB). */
    static constexpr size_t kConfigROMSize = 1024;
    static constexpr size_t kMaxQuadlets = kConfigROMSize / sizeof(uint32_t);

    ConfigROMBuilder();

    /** @brief Legacy single-shot builder to generate a basic Config ROM. */
    void Build(uint32_t busOptions, uint64_t guid, uint32_t nodeCapabilities,
               std::string_view vendorName);

    /**
     * @brief Starts the creation of a staged Config ROM.
     *
     * Initializes the ROM structure and writes the Bus Information Block (BIB).
     */
    void Begin(uint32_t busOptions, uint64_t guid, uint32_t nodeCapabilities);

    /**
     * @brief Adds an immediate entry to the Root Directory.
     * @param keyId The directory entry key ID (low 6 bits of the key byte).
     * @param value24 The 24-bit immediate value.
     * @return True if successful, false if ROM is full or not started.
     */
    bool AddImmediateEntry(uint8_t keyId, uint32_t value24);

    /**
     * @brief Appends a textual descriptor leaf and links it in the Root Directory.
     * @param keyId The directory entry key ID (low 6 bits of the key byte).
     *              For a textual descriptor leaf, this is typically 0x01
     *              (ASFW::FW::ConfigKey::kTextualDescriptor).
     * @param text The ASCII string to store in the leaf ("Minimal ASCII" per IEEE 1212).
     * @return Handle to the created leaf, or an invalid handle if it failed.
     */
    LeafHandle AddTextLeaf(uint8_t keyId, std::string_view text);

    /**
     * @brief Finalizes the Configuration ROM structure.
     *
     * Computes the CRC for the Root Directory and finishes the ROM construction.
     * Uses the ITU-T CRC-16 algorithm specified in IEEE 1212 §7.3.
     */
    void Finalize();

    /**
     * @brief Updates the generation count field in the Bus Information Block.
     * @param generation The new 8-bit generation value (IEEE 1394a-2000).
     */
    void UpdateGeneration(uint8_t generation);

    /**
     * @brief Returns a big-endian (wire-order) view of the ROM image.
     * @return A span of quadlets in big-endian byte order (byte-for-byte export).
     */
    std::span<const uint32_t> ImageBE() const;

    /**
     * @brief Returns the host-order logical ROM image.
     *
     * This is the canonical representation used by ConfigROMStager when copying
     * the image into the OHCI shadow-ROM DMA buffer.
     */
    std::span<const uint32_t> ImageNative() const;

    /** @brief Returns the total number of quadlets in the generated ROM. */
    size_t QuadletCount() const { return quadCount_; }

    /** @brief Extracts the first quadlet (Header) of the Bus Info Block. */
    uint32_t HeaderQuad() const;
    /** @brief Extracts the third quadlet (Bus Options) of the Bus Info Block. */
    uint32_t BusInfoQuad() const;
    /** @brief Extracts the fourth quadlet (GUID High) of the Bus Info Block. */
    uint32_t GuidHiQuad() const;
    /** @brief Extracts the fifth quadlet (GUID Low) of the Bus Info Block. */
    uint32_t GuidLoQuad() const;

  private:
    void Reset();
    void Append(uint32_t value);
    uint16_t ComputeCRC(size_t start, size_t count) const;
    static uint16_t CRCStep(uint16_t crc, uint16_t data);
    static uint32_t MakeDirectoryEntry(uint8_t key, uint8_t type, uint32_t value);

    void FinaliseBIB();
    void FinaliseRootDirectory();
    LeafHandle WriteTextLeaf(std::string_view text);
    bool EnsureRootDirectory();

    std::array<uint32_t, kMaxQuadlets> words_{};           // host-endian logical image
    mutable std::array<uint32_t, kMaxQuadlets> beImage_{}; // scratch for BE view
    size_t quadCount_{0};
    std::optional<size_t> rootDirHeaderIndex_; // root dir header index once started
    uint32_t lastBusOptions_{0};
    bool begun_{false};
    bool finalized_{false};
};

} // namespace ASFW::Driver
