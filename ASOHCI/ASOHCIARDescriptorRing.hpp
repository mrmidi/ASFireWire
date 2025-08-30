#pragma once
//
// ASOHCIARDescriptorRing.hpp
// RX buffer ring + INPUT_MORE/INPUT_LAST descriptors
//
// Spec refs: OHCI 1.1 §8.1 (AR programs), §8.4 (buffer-fill), §3.1.2 (Branch/Z)

#include <DriverKit/IOReturn.h>
#include <stdint.h>

class IOPCIDevice;
class IOBufferMemoryDescriptor;
class IOMemoryMap;
class IODMACommand;

#include "ASOHCIARTypes.hpp"

// RAII helper; implementation hidden behind pimpl
class ASOHCIARDescriptorRing {
public:
  ASOHCIARDescriptorRing() = default;
  ~ASOHCIARDescriptorRing();

  // Allocate N receive buffers (each quadlet-aligned), DMA-map them,
  // and build a linked descriptor chain (ring or list) for AR.
  kern_return_t Initialize(IOPCIDevice *pci, uint32_t bufferCount,
                           uint32_t bufferBytes, ARBufferFillMode fillMode);

  kern_return_t Deallocate();

  // CommandPtr seed to arm the context: returns first descriptor address and Z
  // (§3.1.2)
  kern_return_t GetCommandPtrSeed(uint32_t *outDescriptorAddress,
                                  uint8_t *outZ) const;

  // Scan descriptors for completions and expose views one-by-one.
  // Returns false if nothing is ready.
  bool TryPopCompleted(ARPacketView *outView, uint32_t *outRingIndex);

  // Recycle a buffer after the consumer is done with it (re-arms INPUT_MORE)
  kern_return_t Recycle(uint32_t ringIndex);

  // Useful after bus reset: re-arm CommandPtr and residuals
  kern_return_t ReArmAfterBusReset();

  // Telemetry
  uint32_t BufferCount() const;
  uint32_t BufferBytes() const;
  ARBufferFillMode FillMode() const;

private:
  struct Impl;
  Impl *_impl = nullptr; // allocated in Initialize

  ASOHCIARDescriptorRing(const ASOHCIARDescriptorRing &) = delete;
  ASOHCIARDescriptorRing &operator=(const ASOHCIARDescriptorRing &) = delete;
};
