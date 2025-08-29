//
// ASOHCIITContext.cpp
// Isochronous Transmit (IT) context skeleton (DriverKit-friendly)
//
// Spec anchors:
//   Host interrupt + IsoXmit event/mask registers: OHCI 1.1 Chapter 6 (esp. §6.1 event bits, §6.3 demux)
//   IT DMA programs & descriptor usage: §9.1
//   IT Context registers / cycle match fields: §9.2
//   Safe program appending (tail patching semantics): §9.4
//   IT interrupt meanings (underrun, handling late packets, cycle inconsistent): §9.5
//   IT data / header emission (speed/tag/channel/sy, length): §9.6
//

#include "ASOHCIITContext.hpp"
#include "Shared/ASOHCIContextBase.hpp"
#include "Shared/ASOHCITypes.hpp"
#include "LogHelper.hpp"
#include "OHCIConstants.hpp"

#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>

kern_return_t ASOHCIITContext::Initialize(IOPCIDevice* pci,
                                          uint8_t barIndex,
                                          uint32_t ctxIndex)
{
    if (!pci) return kIOReturnBadArgument;
    _ctxIndex = ctxIndex;
    _policy = {};
    _last = {};

    // Compute per-context register offsets (OHCI 1.1 §9.2)
    ASContextOffsets offs{};
    offs.contextControlSet   = kOHCI_IsoXmitContextControlSet(_ctxIndex);
    offs.contextControlClear = kOHCI_IsoXmitContextControlClear(_ctxIndex);
    offs.commandPtr          = kOHCI_IsoXmitCommandPtr(_ctxIndex);
    return ASOHCIContextBase::Initialize(pci, barIndex, ASContextKind::kIT_Transmit, offs);
}

void ASOHCIITContext::ApplyPolicy(const ITPolicy& policy)
{
    _policy = policy;
    os_log(ASLog(), "IT%u: ApplyPolicy cycleMatch=%u startOnCycle=%u dropIfLate=%u underrunBudgetUs=%u",
           _ctxIndex, policy.cycleMatchEnable, policy.startOnCycle, policy.dropIfLate, policy.underrunBudgetUs);
}

kern_return_t ASOHCIITContext::Enqueue(const ITDesc::Program& program,
                                       const ITQueueOptions& opts)
{
    (void)opts;
    if (!program.headPA || program.descCount == 0) return kIOReturnBadArgument;
    // TODO: implement safe tail-append or initial CommandPtr arm (§9.1 / §9.4)
    os_log_debug(ASLog(), "IT%u: Enqueue head=0x%x z=%u count=%u (stub)",
                 _ctxIndex, program.headPA, program.zHead, program.descCount);
    return kIOReturnUnsupported;
}

void ASOHCIITContext::OnInterruptTx()
{
    // TODO: drain completions for this context (§9.5 using per‑context IsoXmitIntEvent bit from Chapter 6)
    os_log_debug(ASLog(), "IT%u: OnInterruptTx (stub)", _ctxIndex);
}

void ASOHCIITContext::OnCycleInconsistent()
{
    // TODO: stop, delay ≥2 cycles, re-arm cycle match (§9.5 + general cycle timer consistency §5.13)
    os_log(ASLog(), "IT%u: CycleInconsistent (stub)", _ctxIndex);
}

void ASOHCIITContext::RecoverDeadContext()
{
    // Basic policy: clear run, reset counters; higher-level manager may re-init.
    ASOHCIContextBase::RecoverDeadContext();
}

