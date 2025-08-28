// SelfIDManager.cpp
// Implementation: Self-ID DMA setup and IRQ-time completion handling

#include "SelfIDManager.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "OHCIConstants.hpp"
#include "SelfIDDecode.hpp"

kern_return_t SelfIDManager::Initialize(::IOPCIDevice* pci, uint8_t barIndex, uint32_t bufferBytes)
{
  if (!pci || bufferBytes == 0) return kIOReturnBadArgument;
  _pci = pci;
  _bar = barIndex;
  _bufBytes = bufferBytes;

  kern_return_t kr = ::IOBufferMemoryDescriptor::Create(
      kIOMemoryDirectionIn, // device writes into it
      bufferBytes,
      16, // alignment (quadlet)
      &_buf);
  if (kr != kIOReturnSuccess || !_buf) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

  kr = _buf->CreateMapping(0, 0, 0, 0, 0, &_map);
  if (kr != kIOReturnSuccess || !_map) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

  IODMACommandSpecification spec{};
  spec.options = kIODMACommandSpecificationNoOptions;
  spec.maxAddressBits = 32;
  kr = ::IODMACommand::Create(_pci, kIODMACommandCreateNoOptions, &spec, &_dma);
  if (kr != kIOReturnSuccess || !_dma) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

  uint64_t flags = 0;
  uint32_t segCount = 32;
  // We allocate a temporary array and copy the first entry address
  ::IOAddressSegment segs[32] = {};
  kr = _dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                           _buf,
                           0,
                           bufferBytes,
                           &flags,
                           &segCount,
                           segs);
  if (kr != kIOReturnSuccess || segCount < 1 || segs[0].address == 0) {
    if (_dma) { _dma->CompleteDMA(kIODMACommandCompleteDMANoOptions); _dma->release(); _dma = nullptr; }
    return (kr != kIOReturnSuccess) ? kr : kIOReturnNoResources;
  }
  // Store only the first segment pointer (address/length) by allocating one slot.
  _seg = (::IOAddressSegment*)IOMalloc(sizeof(::IOAddressSegment));
  if (!_seg) return kIOReturnNoMemory;
  *_seg = segs[0];

  // Program initial buffer pointer (do not arm yet)
  programSelfIDBuffer();
  _armed = false;
  _inProgress = false;
  _lastGeneration = 0;

  return kIOReturnSuccess;
}

void SelfIDManager::Teardown()
{
  // Scrub registers
  if (_pci) {
    _pci->MemoryWrite32(_bar, kOHCI_SelfIDCount, 0);
    _pci->MemoryWrite32(_bar, kOHCI_SelfIDBuffer, 0);
  }

  if (_dma) { _dma->CompleteDMA(kIODMACommandCompleteDMANoOptions); _dma->release(); _dma = nullptr; }
  if (_map) { _map->release(); _map = nullptr; }
  if (_buf) { _buf->release(); _buf = nullptr; }
  if (_seg) { IOFree(_seg, sizeof(::IOAddressSegment)); _seg = nullptr; }

  _bufBytes = 0;
  _armed = false;
  _inProgress = false;
  _lastGeneration = 0;
  _pci = nullptr;
}

kern_return_t SelfIDManager::Arm(bool clearCount)
{
  if (!_pci || !_seg) return kIOReturnNotReady;
  programSelfIDBuffer();
  if (clearCount) {
    _pci->MemoryWrite32(_bar, kOHCI_SelfIDCount, 0);
  }
  _pci->MemoryWrite32(_bar, kOHCI_LinkControlSet, (kOHCI_LC_RcvSelfID | kOHCI_LC_RcvPhyPkt));
  _armed = true;
  _inProgress = true;
  return kIOReturnSuccess;
}

void SelfIDManager::programSelfIDBuffer()
{
  if (_pci && _seg) {
    _pci->MemoryWrite32(_bar, kOHCI_SelfIDBuffer, static_cast<uint32_t>(_seg->address));
  }
}

uint64_t SelfIDManager::BufferIOVA() const
{
  return (_seg) ? _seg->address : 0;
}

void SelfIDManager::verifyGenerationAndDispatch(uint32_t countReg)
{
  // Extract generation and size
  uint32_t gen1 = (countReg & kOHCI_SelfIDCount_selfIDGeneration) >> 16;
  uint32_t sizeQuads = (countReg & kOHCI_SelfIDCount_selfIDSize) >> 2; // size in bytes >>2
  bool error = (countReg & kOHCI_SelfIDCount_selfIDError) != 0;

  if (!_map || error || sizeQuads == 0) {
    _inProgress = false;
    return;
  }

  const uint32_t* buf = reinterpret_cast<const uint32_t*>((uintptr_t)_map->GetAddress());
  uint32_t lenQuads = static_cast<uint32_t>(_map->GetLength() / sizeof(uint32_t));
  if (sizeQuads > lenQuads) sizeQuads = lenQuads; // clamp

  // Decode now (can be heavy). If needed, the caller can route to default queue instead.
  SelfID::Result res = SelfID::Decode(buf, sizeQuads);
  // Prefer generation from count register as authoritative.
  res.generation = gen1;

  if (_onDecode) _onDecode(res);

  // Re-read generation to detect mid-decode resets
  uint32_t count2 = 0; _pci->MemoryRead32(_bar, kOHCI_SelfIDCount, &count2);
  uint32_t gen2 = (count2 & kOHCI_SelfIDCount_selfIDGeneration) >> 16;
  if (gen1 == gen2) {
    _lastGeneration = gen1;
    if (_onStable) _onStable(res);
  }

  _inProgress = false;
}

void SelfIDManager::OnSelfIDComplete(uint32_t selfIDCountRegValue)
{
  verifyGenerationAndDispatch(selfIDCountRegValue);
}
