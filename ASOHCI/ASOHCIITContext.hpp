#pragma once
//
// ASOHCIITContext.hpp
// Per-IT-context plumbing built on ASOHCIContextBase.
//
// Spec refs: OHCI 1.1 IT §6.2 (context registers), §6.1 (program),
//            §6.3 (interrupts/cycleInconsistent), §6.4 (data/late/underrun)

#include <stdint.h>
#include "Shared/ASOHCIContextBase.hpp"
#include "ASOHCIITTypes.hpp"
#include "ASOHCIITDescriptor.hpp"
#include "ASOHCIITStatus.hpp"

class ASOHCIITContext : public ASOHCIContextBase {
public:
    // ctxIndex: hardware IT context number (0..N-1). Offsets computed in .cpp (§6.2)
    virtual kern_return_t Initialize(IOPCIDevice* pci,
                                     uint8_t barIndex,
                                     uint32_t ctxIndex);

    virtual void ApplyPolicy(const ITPolicy& policy);

    // Enqueue one packet program (may append while active if policy allows) (§6.1)
    virtual kern_return_t Enqueue(const ITDesc::Program& program,
                                  const ITQueueOptions& opts);

    // Called by manager when isoXmitIntEvent indicates this context fired (§6.3)
    virtual void OnInterruptTx();

    // Manager signals cycleInconsistent to cycle-matched contexts (§6.3)
    virtual void OnCycleInconsistent();

    // Expose some counters/telemetry
    uint32_t PacketsInFlight() const { return _outstanding; }

protected:
    virtual void RecoverDeadContext() override; // skip overflow / unrecoverable (§6.3)

private:
    uint32_t _ctxIndex = 0;
    ITPolicy _policy{};
    ITCompletion _last{};
};

