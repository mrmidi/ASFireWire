#pragma once
//
// ASOHCIRegisterIO.hpp
// Lightweight wrapper for OHCI MMIO register access.
// Provides Read/Write/Set/Clear helpers reusable by controller and subsystems.
//

#include <DriverKit/IOLib.h>
#include <DriverKit/OSObject.h>
#include <PCIDriverKit/IOPCIDevice.h>

class ASOHCIRegisterIO : public OSObject {

public:
  // Initialize with PCI device and BAR index (MMIO BAR)
  virtual bool Init(IOPCIDevice *pci, uint8_t barIndex);

  // Basic 32-bit accessors
  virtual kern_return_t Read32(uint32_t offset, uint32_t *outValue) const;
  virtual void Write32(uint32_t offset, uint32_t value) const;

  // Bit set/clear helpers (write-1-to-set/clear registers)
  virtual void Set32(uint32_t offset, uint32_t mask) const;
  virtual void Clear32(uint32_t offset, uint32_t mask) const;

  // Read-modify-write utility: (val & ~clearMask) | setMask
  virtual void ReadModifyWrite32(uint32_t offset, uint32_t clearMask,
                                 uint32_t setMask) const;

  // Accessors
  IOPCIDevice *GetPCIDevice() const { return _pci; }
  uint8_t GetBAR() const { return _bar; }

public:
  // Static creation method for DriverKit compatibility
  static ASOHCIRegisterIO *Create() {
    return new (IOMalloc(sizeof(ASOHCIRegisterIO))) ASOHCIRegisterIO();
  }

private:
  IOPCIDevice *_pci = nullptr; // not retained; owned by controller
  uint8_t _bar = 0;
};
