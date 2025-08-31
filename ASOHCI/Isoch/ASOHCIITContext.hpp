#pragma once
//
// ASOHCIITContext.hpp
// Per-IT-context plumbing built on ASOHCIContextBase.
//
// Spec refs (OHCI 1.1):
//   §9.1 IT DMA programs (descriptor forms, initial arm)
//   §9.2 IT Context registers (cycleMatch fields)
//   §9.4 Appending (safe tail patch rules)
//   §9.5 Interrupts (IsoXmit events, underrun handling, late synthesis)
//   §9.6 Data format (header emission via Immediate descriptors)
//   Chapter 6 for host IntEvent / IsoXmitIntEvent register demux

#include "../Shared/ASOHCIContextBase.hpp"
#include "ASOHCIITDescriptor.hpp"
#include "ASOHCIITStatus.hpp"
#include "ASOHCIITTypes.hpp"
#include <stdint.h>

class ASOHCIITContext : public ASOHCIContextBase {
public:
  ASOHCIITContext() = default;
  virtual ~ASOHCIITContext() = default;

  // ctxIndex: hardware IT context number (0..N-1). Offsets computed in .cpp
  // (see §9.2)
  virtual kern_return_t Initialize(IOPCIDevice *pci, uint8_t barIndex,
                                   uint32_t ctxIndex);

  // Override Start: do not write run + empty CommandPtr; instead just clear any
  // stale run and wait for first Enqueue.
  virtual kern_return_t Start() override;

  virtual void ApplyPolicy(const ITPolicy &policy);

  // Enqueue one packet program (may append while active if policy allows)
  // (§9.1, §9.4)
  virtual kern_return_t Enqueue(const ITDesc::Program &program,
                                const ITQueueOptions &opts);

  // Called by manager when isoXmitIntEvent indicates this context fired (§9.5;
  // demux via Chapter 6)
  virtual void OnInterruptTx();

  // Manager signals cycleInconsistent to cycle-matched contexts (§9.5 cycle
  // inconsistent handling)
  virtual void OnCycleInconsistent();

  // Expose some counters/telemetry
  uint32_t PacketsInFlight() const { return _outstanding; }

protected:
  virtual void
  RecoverDeadContext() override; // skip overflow / unrecoverable (§9.5)

private:
  uint32_t _ctxIndex = 0;
  ITPolicy _policy{};
  ITCompletion _last{};
  uint32_t _outstanding = 0;

  // Simple ring of in-flight programs (pending completion). Size small due to
  // limited pipeline (§9.4 guidance)
  struct InFlightProg {
    uint32_t headPA;
    uint32_t tailPA;
    void *tailVA;
    uint8_t zHead;
    bool valid;
    uint16_t lastStatus =
        0; // decoded status field from tail descriptor (controller-written)
    uint16_t timestamp = 0; // optional cycle/timestamp if available
  };
  static constexpr uint32_t kMaxInFlight = 16; // adjustable
  InFlightProg _ring[kMaxInFlight] = {};
  uint32_t _ringHead = 0; // next insertion
  uint32_t _ringTail = 0; // next retirement
  bool _ringFull = false;

  InFlightProg *CurrentTail();
  void PushProgram(const ITDesc::Program &p);
  void RetireOne();
};
