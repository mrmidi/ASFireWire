// ASOHCIConfigROM.hpp
#pragma once

#include <stdint.h>
#include <vector>

// IEEE 1212 Config ROM builder (DriverKit-friendly, no locks)
// Builds a general-format BIB (5 quadlets) and a minimal root directory.
class ASOHCIConfigROM {
public:
    // Entry type (IEEE 1212 directory entry)
    enum EntryType : uint8_t {
        IMMEDIATE  = 0,
        CSR_OFFSET = 1,
        LEAF       = 2,
        DIRECTORY  = 3,
    };

    // Build from OHCI hardware values.
    // - busOptions: value to mirror in BIB[2] and optionally in root dir entry
    // - guidHi/Lo: used to compute EUI-64 and derive Vendor_ID (top 24 bits)
    bool buildFromHardware(uint32_t busOptions, uint32_t guidHi, uint32_t guidLo,
                           bool includeRootDirectory = true, bool includeNodeCapsEntry = true);

    // Write big-endian ROM image to a destination buffer; zero-fill remaining bytes
    void writeToBufferBE(void* dst, size_t bytes) const;

    // Accessors
    const std::vector<uint32_t>& image() const { return _quads; }
    uint32_t totalLengthBytes() const { return (uint32_t)(_quads.size() * 4); }
    uint32_t headerQuad() const { return _quads.size() > 0 ? _quads[0] : 0; }
    uint32_t busOptionsQuad() const { return _quads.size() > 2 ? _quads[2] : 0; }
    uint32_t romQuad(size_t idx) const { return idx < _quads.size() ? _quads[idx] : 0; }
    uint32_t vendorID() const { return _vendorID; }
    uint64_t eui64() const { return _eui64; }

private:
    // Internal helpers
    static uint32_t makeDirEntry(EntryType type, uint8_t key, uint32_t value);
    static uint16_t crc16_for_doublet(uint16_t crc, uint16_t data);
    static uint16_t computeCRC16(const std::vector<uint32_t>& quads, size_t startIdx, size_t quadCount);
    static uint32_t bswap32(uint32_t x);

    void buildBIB(uint32_t busOptions, uint32_t guidHi, uint32_t guidLo);
    void startRootDirectory();
    void finishRootDirectory();
    void addRootImmediate(uint8_t key, uint32_t value);
    // Leaf/directory offsets may be added later as needed

private:
    std::vector<uint32_t> _quads; // host-endian logical image before BE writeout
    size_t _rootDirStart = 0;     // index where root directory header lives
    uint64_t _eui64 = 0;
    uint32_t _vendorID = 0;       // top 24 bits of EUI-64
};
