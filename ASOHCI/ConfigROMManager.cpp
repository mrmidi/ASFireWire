// ConfigROMManager.cpp
// Implementation: Build/map local Config ROM and commit header on BusReset

#include "ConfigROMManager.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "OHCIConstants.hpp"
#include "ASOHCIConfigROM.hpp"

#include "LogHelper.hpp"

 

kern_return_t ConfigROMManager::Initialize(::IOPCIDevice* pci, uint8_t barIndex,
                      uint32_t busOptions, uint32_t guidHi, uint32_t guidLo,
                      uint32_t romBytes)
{
  if (!pci || romBytes == 0) return kIOReturnBadArgument;
  _pci = pci; _bar = barIndex; _busOptions = busOptions;

  kern_return_t kr = ::IOBufferMemoryDescriptor::Create(
      kIOMemoryDirectionOut, // device reads from it
      romBytes,
      4,
      &_buf);
  if (kr != kIOReturnSuccess || !_buf) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

  kr = _buf->CreateMapping(0, 0, 0, 0, 0, &_map);
  if (kr != kIOReturnSuccess || !_map) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

  // Build ROM into mapped memory and stage header/bus options
  buildAndStage(busOptions, guidHi, guidLo, romBytes);

  // DMA map
  IODMACommandSpecification spec{};
  spec.options = kIODMACommandSpecificationNoOptions;
  spec.maxAddressBits = 32;
  kr = ::IODMACommand::Create(_pci, kIODMACommandCreateNoOptions, &spec, &_dma);
  if (kr != kIOReturnSuccess || !_dma) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

  uint64_t flags = 0; uint32_t segCount = 32; ::IOAddressSegment segs[32] = {};
  kr = _dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, _buf, 0, romBytes, &flags, &segCount, segs);
  if (kr != kIOReturnSuccess || segCount < 1 || segs[0].address == 0) {
    _dma->CompleteDMA(kIODMACommandCompleteDMANoOptions); _dma->release(); _dma = nullptr;
    return (kr != kIOReturnSuccess) ? kr : kIOReturnNoResources;
  }
  _seg = (::IOAddressSegment*)IOMalloc(sizeof(::IOAddressSegment));
  if (!_seg) return kIOReturnNoMemory;
  *_seg = segs[0];

  // Program map address and mirror BusOptions; leave header staged for BusReset
  programROMMap();

  // Log Config ROM creation/mapping summary
  os_log(ASLog(), "ASOHCI: ConfigROM built: vendor=0x%06x EUI64=%08x%08x",
         _vendorId & 0xFFFFFF, (unsigned)((_eui64 >> 32) & 0xFFFFFFFFu), (unsigned)(_eui64 & 0xFFFFFFFFu));
  if (_seg) {
    os_log(ASLog(), "ASOHCI: ConfigROM mapped IOVA=0x%08x BusOptions=0x%08x Header(staged)=0x%08x",
           (unsigned)_seg->address, _busOptions, _headerQuad);
  }

  return kIOReturnSuccess;
}

void ConfigROMManager::buildAndStage(uint32_t busOptions, uint32_t guidHi, uint32_t guidLo, uint32_t romBytes)
{
  if (!_map) return;
  // Build ROM (big-endian write) using existing builder
  ASOHCIConfigROM rom;
  rom.buildFromHardware(busOptions, guidHi, guidLo, /*includeRootDirectory*/true, /*includeNodeCaps*/true);
  void* romPtr = (void*)_map->GetAddress();
  size_t romLen = (size_t)_map->GetLength();
  rom.writeToBufferBE(romPtr, romLen);

  _headerQuad = rom.headerQuad();
  _busOptions = rom.romQuad(2);
  _headerNeedsCommit = true;

  // Save identity (optional)
  _eui64 = ((uint64_t)guidHi << 32) | (uint64_t)guidLo;
  _vendorId = rom.vendorID();
}

void ConfigROMManager::programROMMap()
{
  if (!_pci || !_seg) return;
  _pci->MemoryWrite32(_bar, kOHCI_ConfigROMmap, static_cast<uint32_t>(_seg->address));
  _pci->MemoryWrite32(_bar, kOHCI_BusOptions, _busOptions);
  // Do not write header yet; commit on bus reset
}

void ConfigROMManager::CommitOnBusReset()
{
  if (!_pci || !_headerNeedsCommit || _headerQuad == 0) return;
  _pci->MemoryWrite32(_bar, kOHCI_BusOptions, _busOptions);
  _pci->MemoryWrite32(_bar, kOHCI_ConfigROMhdr, _headerQuad);
  _headerNeedsCommit = false;
}

void ConfigROMManager::Teardown()
{
  if (_pci) {
    _pci->MemoryWrite32(_bar, kOHCI_ConfigROMmap, 0);
  }
  if (_dma) {
    _dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    _dma->release(); _dma = nullptr;
  }
  if (_map) { _map->release(); _map = nullptr; }
  if (_buf) { _buf->release(); _buf = nullptr; }
  if (_seg) { IOFree(_seg, sizeof(::IOAddressSegment)); _seg = nullptr; }
  _headerQuad = 0; _busOptions = 0; _headerNeedsCommit = false; _eui64 = 0; _vendorId = 0;
}

void ConfigROMManager::Dump(const char* /*label*/) const
{
  // Intentionally left as a lightweight no-op to avoid introducing a shared hex dump dependency.
}

uint64_t ConfigROMManager::ROMIOVA() const { return (_seg) ? _seg->address : 0; }
