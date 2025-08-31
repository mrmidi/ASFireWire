#pragma once
//
// ASOHCIContextBase.hpp
// Shared context register plumbing (Start/Stop/Wake/CommandPtr)
//
// Spec refs: OHCI 1.1 §3.1.1 (ContextControl run/active/dead/wake),
//            §3.1.2 (CommandPtr: [31:4] addr, [3:0] Z),
//            AT §7.2 (AT context registers), AR §8.2 (AR context registers)
//

#include "ASOHCITypes.hpp"
#include "OHCIConstants.hpp"
#include <DriverKit/IOReturn.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <atomic>

class ASOHCIContextBase {

public:
  ASOHCIContextBase() = default;
  virtual ~ASOHCIContextBase() = default;

  // Initialize with PCI device, BAR index, context kind, and precomputed
  // offsets
  virtual kern_return_t Initialize(IOPCIDevice *pci, uint8_t barIndex,
                                   ASContextKind kind,
                                   const ASContextOffsets &offsets);

  // Start/Stop/Wake the context (§3.1.1.*)
  virtual kern_return_t Start();
  virtual kern_return_t Stop();
  virtual kern_return_t Wake();

  // Set device gone flag for safe MMIO access during teardown
  void SetDeviceGone(bool gone) {
    _deviceGone.store(gone, std::memory_order_release);
  }

  // Bus reset lifecycle hooks (common policy)
  virtual void OnBusResetBegin();
  virtual void OnBusResetEnd();

  // Status helpers
  virtual bool IsRunning() const;
  virtual bool IsActive() const;
  virtual uint32_t
  ReadContextSet() const; // reads ContextControl (via read address)

  // CommandPtr writer (§3.1.2): addr must be 16-byte aligned; z in [0..15]
  virtual kern_return_t WriteCommandPtr(uint32_t descriptorAddress,
                                        uint8_t zNibble);

  // Accessors
  ASContextKind GetKind() const { return _kind; }
  const ASContextOffsets &GetOffsets() const { return _offs; }
  uint8_t GetBAR() const { return _bar; }
  IOPCIDevice *GetPCIDevice() const { return _pci; }

protected:
  // Low-level register IO (Memory BAR space, not PCI config)
  virtual void WriteContextSet(uint32_t value);
  virtual void WriteContextClear(uint32_t value);

  // Allow derived types to override recovery strategy
  virtual void RecoverDeadContext();

protected:
  IOPCIDevice *_pci = nullptr;
  uint8_t _bar = 0;
  ASContextKind _kind = ASContextKind::kAT_Request;
  ASContextOffsets _offs{};

  // Device removal safety flag
  std::atomic<bool> _deviceGone{false};

  // lightweight counters useful for both directions
  uint32_t _outstanding = 0;
  uint32_t _outstandingCap = 1;

private:
  // noncopyable
  ASOHCIContextBase(const ASOHCIContextBase &) = delete;
  ASOHCIContextBase &operator=(const ASOHCIContextBase &) = delete;
};
