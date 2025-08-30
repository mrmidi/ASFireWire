#pragma once
//
// ASOHCIARContext.hpp
// AR Request/Response context wrappers on top of ASOHCIContextBase
//
// Spec refs: OHCI 1.1 §8.2 (AR context registers), §8.1 (program rules),
//            §8.4 (buffer-fill), §8.6 (interrupts)

#include <DriverKit/IOReturn.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "ASOHCIARTypes.hpp"
#include "Shared/ASOHCIContextBase.hpp"
#include "Shared/ASOHCITypes.hpp"

class ASOHCIARDescriptorRing;
class ASOHCIARStatus;

class ASOHCIARContext : public ASOHCIContextBase {
public:
  // Back-compat enum to satisfy existing call sites
  enum ContextType { AR_REQUEST_CONTEXT = 0, AR_RESPONSE_CONTEXT = 1 };
  ASOHCIARContext() = default;
  ~ASOHCIARContext() = default;

  // role selects Request vs Response; offsets must be filled per role (§8.2)
  kern_return_t Initialize(IOPCIDevice *pci, uint8_t barIndex,
                           ARContextRole role, const ASContextOffsets &offsets,
                           ARBufferFillMode fillMode);

  // Back-compat initializer that also builds and owns a ring
  kern_return_t Initialize(IOPCIDevice *pci, ContextType contextType,
                           uint8_t barIndex, uint32_t bufferCount,
                           uint32_t bufferBytes);

  // Back-compat minimal init: uses a default buffer policy
  kern_return_t Initialize(IOPCIDevice *pci, ContextType contextType,
                           uint8_t barIndex);

  // Attach a prepared ring (Initialize it first)
  kern_return_t AttachRing(ASOHCIARDescriptorRing *ring);

  // Start/Stop override calls base + arms CommandPtr from ring seed
  kern_return_t Start() override;
  kern_return_t Stop() override;

  // Interrupt entrypoints (called by router per §8.6)
  void OnPacketArrived();  // “packet available” style
  void OnBufferComplete(); // “buffer filled/last” style

  // Back-compat ISR hook
  kern_return_t HandleInterrupt();

  // Consumer API — pull one packet, parse elsewhere
  bool TryDequeue(ARPacketView *outView, uint32_t *outRingIndex);

  // Recycle after consumer processed it
  kern_return_t Recycle(uint32_t ringIndex);

  // Status helper (optional)
  void SetStatusHelper(ASOHCIARStatus *status);

  // Fill mode
  ARBufferFillMode GetFillMode() const { return _fill; }

private:
  ASOHCIARDescriptorRing *_ring = nullptr; // not owned
  ASOHCIARStatus *_stat = nullptr;         // not owned
  ARContextRole _role = ARContextRole::kRequest;
  ARBufferFillMode _fill = ARBufferFillMode::kImmediate;
};
