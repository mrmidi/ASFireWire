#pragma once
//
// ASOHCIITManager.hpp
// Owns multiple IT contexts, descriptor pool sharing, and interrupt fan-out.
//
// Spec refs (OHCI 1.1): Chapter 6 (IsoXmitIntEvent demux), §9.2 (context
// discovery),
//   §9.4 (safe appending to a running program), §9.5 (interrupt semantics)

#include "../Async/ASOHCIATDescriptorPool.hpp"
#include "ASOHCIITContext.hpp"
#include "ASOHCIITDescriptor.hpp"
#include "ASOHCIITProgramBuilder.hpp"
#include "ASOHCIITTypes.hpp"
#include <DriverKit/OSSharedPtr.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <memory>

class ASOHCIITManager {
public:
  ASOHCIITManager() = default;
  ~ASOHCIITManager() = default;

  // Discover available IT contexts, init shared pool, apply defaults.
  virtual kern_return_t Initialize(OSSharedPtr<IOPCIDevice> pci,
                                   uint8_t barIndex,
                                   const ITPolicy &defaultPolicy);

  virtual kern_return_t StartAll();
  virtual kern_return_t StopAll();

  // Queue a packet into a specific IT context
  virtual kern_return_t Queue(uint32_t ctxId, ITSpeed spd, uint8_t tag,
                              uint8_t channel, uint8_t sy,
                              const uint32_t *payloadPAs,
                              const uint32_t *payloadSizes, uint32_t fragments,
                              const ITQueueOptions &opts);

  // Top-half: called from the device’s main ISR after reading host IntEvent
  // (§6.3)
  virtual void OnInterrupt_TxEventMask(uint32_t mask);
  virtual void
  OnInterrupt_CycleInconsistent(); // host bit -> fan-out to cycle-matched

  // Telemetry
  uint32_t NumContexts() const { return _numCtx; }

protected:
  // Probe isoXmitIntMask to figure out how many IT contexts exist (§6.3)
  virtual uint32_t ProbeContextCount();

private:
  OSSharedPtr<IOPCIDevice> _pci;
  uint8_t _bar = 0;

  std::unique_ptr<ASOHCIITContext>
      _ctx[32]; // upper bound; actual count discovered
  uint32_t _numCtx = 0;

  std::unique_ptr<ASOHCIITProgramBuilder> _builder;
  std::unique_ptr<ASOHCIATDescriptorPool> _pool; // shared pool reused from AT
  ITPolicy _defaultPolicy{};
};
