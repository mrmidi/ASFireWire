//
// ConfigROMManager.hpp
// Centralizes local node Config ROM build, mapping, and commit
//
// Responsibilities:
//   • Build BIB + minimal root directory (via ASOHCIConfigROM)
//   • Map to 32-bit IOVA and program kOHCI_ConfigROMmap
//   • Mirror kOHCI_BusOptions and commit header on BusReset in correct order
//   • Provide optional hex dump helper for diagnostics
//

#pragma once

#include <stdint.h>
#include <DriverKit/IOLib.h>

// Global forward declarations for DriverKit classes (pointer use only)
class IOPCIDevice;
class IOBufferMemoryDescriptor;
class IOMemoryMap;
class IODMACommand;
struct IOAddressSegment;

// Use global DriverKit class types in :: scope

class ConfigROMManager {
public:
  ConfigROMManager() = default;
  ~ConfigROMManager() = default;

  ConfigROMManager(const ConfigROMManager&) = delete;
  ConfigROMManager& operator=(const ConfigROMManager&) = delete;

  // Allocate/map ROM buffer, build image, DMA-map, and program ROM map register.
  // romBytes usually 1024. Returns kIOReturnSuccess / error (implementation).
  kern_return_t Initialize(::IOPCIDevice* pci, uint8_t barIndex,
                      uint32_t busOptions, uint32_t guidHi, uint32_t guidLo,
                      uint32_t romBytes);

  // Free map/DMA/buffer and scrub ROM map register.
  void Teardown();

  // Call from BusReset handling to atomically commit staged BusOptions and Header.
  void CommitOnBusReset();

  // Optional hexdump; safe no-op if unmapped.
  void Dump(const char* label) const;

  // Telemetry / accessors
  uint64_t EUI64() const { return _eui64; }
  uint32_t VendorId() const { return _vendorId; }
  uint32_t BusOptions() const { return _busOptions; }
  bool     HeaderStaged() const { return _headerNeedsCommit; }
  uint32_t HeaderQuad() const { return _headerQuad; }
  uint64_t ROMIOVA() const; // 0 if not ready

private:
  void buildAndStage(uint32_t busOptions, uint32_t guidHi, uint32_t guidLo, uint32_t romBytes);
  void programROMMap();

private:
  // Bound device/regs
  ::IOPCIDevice*             _pci = nullptr;
  uint8_t                    _bar = 0;

  // Owned resources
  ::IOBufferMemoryDescriptor*  _buf = nullptr;
  ::IOMemoryMap*               _map = nullptr;
  ::IODMACommand*              _dma = nullptr;
  ::IOAddressSegment*          _seg = nullptr;

  // Staged BIB/header data
  uint32_t                   _headerQuad = 0;
  uint32_t                   _busOptions = 0;
  bool                       _headerNeedsCommit = false;

  // Identity (from GUID)
  uint64_t                   _eui64 = 0;
  uint32_t                   _vendorId = 0;
};
