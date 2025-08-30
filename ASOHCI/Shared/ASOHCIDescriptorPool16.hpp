#pragma once
//
// ASOHCIDescriptorPool16.hpp
// Generic 16-byte descriptor pool for OHCI programs (AT/AR)
//
// Spec refs: OHCI 1.1 §3.1.2 (CommandPtr/Z nibble), AT §7.1 (program/list
// rules),
//            AR §8.1 (program/list rules). Descriptors are 16-byte units,
//            32-bit DMA.
//
#include <DriverKit/OSObject.h>
#include <stdint.h>

class IOPCIDevice;
class IOMemoryMap;
class IOBufferMemoryDescriptor;
class OSArray;

class ASOHCIDescriptorPool16 : public OSObject {
  OSDeclareDefaultStructors(ASOHCIDescriptorPool16)

      public :
      // Allocate a physically contiguous, 32-bit addressable pool.
      // poolSizeBytes must be a multiple of 16.
      virtual kern_return_t
      Initialize(IOPCIDevice *pci, uint32_t poolSizeBytes);

  virtual kern_return_t Deallocate();

  struct Block {
    uint32_t physicalAddress = 0;   // 32-bit IOVA of first 16-byte unit
    void *virtualAddress = nullptr; // CPU VA mapping
    uint32_t unitCount = 0; // number of 16-byte units in this allocation
    uint8_t zValue =
        0; // Z nibble for CommandPtr if this block is used as a single program
    bool valid = false;
  };

  // Allocate ‘unitCount’ 16-byte units (2..8 typical for a single packet
  // program).
  virtual Block AllocateUnits(uint32_t unitCount);

  // Return a previously allocated block
  virtual kern_return_t FreeUnits(const Block &block);

  // Telemetry
  virtual uint32_t GetAvailableUnits() const;
  virtual uint32_t GetTotalUnits() const;
  virtual bool IsInitialized() const { return _initialized; }

private:
  // internal helpers and state (hidden from headers)
  IOPCIDevice *_pci = nullptr;
  IOBufferMemoryDescriptor *_pool = nullptr;
  IOMemoryMap *_map = nullptr;
  OSArray *_freeList = nullptr;
  uint32_t _poolBytes = 0;
  uint32_t _totalUnits = 0;
  uint32_t _physBase = 0;
  void *_virtBase = nullptr;
  bool _initialized = false;

  ASOHCIDescriptorPool16(const ASOHCIDescriptorPool16 &) = delete;
  ASOHCIDescriptorPool16 &operator=(const ASOHCIDescriptorPool16 &) = delete;
};
