#include "ASOHCIRegisterIO.hpp"

// OSDefineMetaClassAndStructors not needed in DriverKit

bool ASOHCIRegisterIO::Init(IOPCIDevice *pci, uint8_t barIndex) {
  _pci = pci;
  _bar = barIndex;
  return (_pci != nullptr);
}

kern_return_t ASOHCIRegisterIO::Read32(uint32_t offset,
                                       uint32_t *outValue) const {
  if (!_pci || !outValue)
    return kIOReturnBadArgument;
  _pci->MemoryRead32(_bar, offset, outValue);
  return kIOReturnSuccess;
}

void ASOHCIRegisterIO::Write32(uint32_t offset, uint32_t value) const {
  if (!_pci)
    return;
  _pci->MemoryWrite32(_bar, offset, value);
}

void ASOHCIRegisterIO::Set32(uint32_t offset, uint32_t mask) const {
  if (!_pci)
    return;
  _pci->MemoryWrite32(_bar, offset, mask);
}

void ASOHCIRegisterIO::Clear32(uint32_t offset, uint32_t mask) const {
  if (!_pci)
    return;
  _pci->MemoryWrite32(_bar, offset, mask);
}

void ASOHCIRegisterIO::ReadModifyWrite32(uint32_t offset, uint32_t clearMask,
                                         uint32_t setMask) const {
  if (!_pci)
    return;
  uint32_t val = 0;
  _pci->MemoryRead32(_bar, offset, &val);
  val &= ~clearMask;
  val |= setMask;
  _pci->MemoryWrite32(_bar, offset, val);
}
