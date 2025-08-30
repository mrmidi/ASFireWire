// ASOHCIConfigROM.cpp
#include "ASOHCIConfigROM.hpp"

// BusName '1394' per IEEE 1212 / Apple IOFireWireFamily
static constexpr uint32_t kFWBIBBusName_1394 = 0x31333934u;

bool ASOHCIConfigROM::buildFromHardware(uint32_t busOptions, uint32_t guidHi,
                                        uint32_t guidLo,
                                        bool includeRootDirectory,
                                        bool includeNodeCapsEntry) {
  _quads.clear();
  _rootDirStart = 0;

  // Compute EUI-64 and derive Vendor_ID (top 24 bits)
  _eui64 = ((uint64_t)guidHi << 32) | (uint64_t)guidLo;
  _vendorID = (uint32_t)((_eui64 >> 40) & 0xFFFFFFu);

  // Build BIB and header
  buildBIB(busOptions, guidHi, guidLo);

  if (includeRootDirectory) {
    startRootDirectory();
    // Vendor_ID (mandatory in root directory) — immediate 24-bit company_id
    addRootImmediate(/*VENDOR key*/ 0x03, _vendorID);
    if (includeNodeCapsEntry) {
      // Node_Capabilities (optional but useful) mirrors BusOptions
      addRootImmediate(/*NODE_CAPABILITIES key*/ 0x0C, busOptions);
    }
    finishRootDirectory();
  }

  return true;
}

uint32_t ASOHCIConfigROM::makeDirEntry(EntryType type, uint8_t key,
                                       uint32_t value) {
  return ((uint32_t)type << 30) | ((uint32_t)(key & 0x3F) << 24) |
         (value & 0x00FFFFFFu);
}

uint16_t ASOHCIConfigROM::crc16_for_doublet(uint16_t crc, uint16_t data) {
  // ITU-T CRC-16 with poly 0x1021, initial crc=0, process MSB-first per doublet
  crc ^= data;
  for (int i = 0; i < 16; ++i) {
    if (crc & 0x8000) {
      crc = (uint16_t)((crc << 1) ^ 0x1021);
    } else {
      crc <<= 1;
    }
  }
  return crc;
}

uint16_t ASOHCIConfigROM::computeCRC16(const std::vector<uint32_t> &quads,
                                       size_t startIdx, size_t quadCount) {
  uint16_t crc = 0;
  for (size_t i = 0; i < quadCount; ++i) {
    uint32_t q = quads[startIdx + i];
    uint16_t hi = (uint16_t)((q >> 16) & 0xFFFF);
    uint16_t lo = (uint16_t)(q & 0xFFFF);
    crc = crc16_for_doublet(crc, hi);
    crc = crc16_for_doublet(crc, lo);
  }
  return crc;
}

uint32_t ASOHCIConfigROM::bswap32(uint32_t x) {
  return (x >> 24) | ((x >> 8) & 0x0000FF00u) | ((x << 8) & 0x00FF0000u) |
         (x << 24);
}

void ASOHCIConfigROM::buildBIB(uint32_t busOptions, uint32_t guidHi,
                               uint32_t guidLo) {
  // General BIB: header + 4 quadlets (bus_name, node_capabilities, guid_hi,
  // guid_lo)
  _quads.reserve(8);
  _quads.push_back(0);                  // header placeholder at [0]
  _quads.push_back(kFWBIBBusName_1394); // [1]
  _quads.push_back(busOptions);         // [2]
  _quads.push_back(guidHi);             // [3]
  _quads.push_back(guidLo);             // [4]

  // Header fields
  const uint8_t bus_info_length =
      4;                        // number of quadlets following header in BIB
  const uint8_t crc_length = 4; // quadlets covered by BIB CRC (quads 1..4)
  uint16_t bib_crc = computeCRC16(_quads, 1, crc_length);
  _quads[0] = ((uint32_t)bus_info_length << 24) | ((uint32_t)crc_length << 16) |
              (uint32_t)bib_crc;
}

void ASOHCIConfigROM::startRootDirectory() {
  // Reserve header for root directory: [len<<16 | crc], both to be filled later
  _rootDirStart = _quads.size();
  _quads.push_back(0); // header placeholder
}

void ASOHCIConfigROM::addRootImmediate(uint8_t key, uint32_t value) {
  _quads.push_back(makeDirEntry(EntryType::IMMEDIATE, key, value));
}

void ASOHCIConfigROM::finishRootDirectory() {
  size_t entries = _quads.size() - (_rootDirStart + 1);
  uint32_t len = (uint32_t)entries; // number of quadlets following header
  // Compute CRC over entries only
  uint16_t dir_crc = computeCRC16(_quads, _rootDirStart + 1, entries);
  _quads[_rootDirStart] = (len << 16) | dir_crc;
}

void ASOHCIConfigROM::writeToBufferBE(void *dst, size_t bytes) const {
  uint8_t *p = static_cast<uint8_t *>(dst);
  size_t quadCount = bytes / 4;
  size_t i = 0;
  for (; i < _quads.size() && i < quadCount; ++i) {
    uint32_t v = _quads[i];
    // Emit big-endian bytes (MSB first) without double-swapping
    p[i * 4 + 0] = (uint8_t)((v >> 24) & 0xFF);
    p[i * 4 + 1] = (uint8_t)((v >> 16) & 0xFF);
    p[i * 4 + 2] = (uint8_t)((v >> 8) & 0xFF);
    p[i * 4 + 3] = (uint8_t)(v & 0xFF);
  }
  for (; i < quadCount; ++i) {
    p[i * 4 + 0] = 0;
    p[i * 4 + 1] = 0;
    p[i * 4 + 2] = 0;
    p[i * 4 + 3] = 0;
  }
}
