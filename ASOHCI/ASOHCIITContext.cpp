//
// ASOHCIITContext.cpp
// Isochronous Transmit (IT) context skeleton (DriverKit-friendly)
//
// Spec anchors: IT registers/behavior (§6.* in OHCI 1.1)
//

#include "ASOHCIITContext.hpp"
#include "Shared/ASOHCIContextBase.hpp"
#include "Shared/ASOHCITypes.hpp"
#include "LogHelper.hpp"

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

    // NOTE: Offsets for IT contexts are not wired yet. Keep as zeroed to compile.
    ASContextOffsets offs{};
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
    // TODO: implement safe tail-append or initial CommandPtr arm (§6.1)
    os_log_debug(ASLog(), "IT%u: Enqueue head=0x%x z=%u count=%u (stub)",
                 _ctxIndex, program.headPA, program.zHead, program.descCount);
    return kIOReturnUnsupported;
}

void ASOHCIITContext::OnInterruptTx()
{
    // TODO: drain completions for this context (§6.3)
    os_log_debug(ASLog(), "IT%u: OnInterruptTx (stub)", _ctxIndex);
}

void ASOHCIITContext::OnCycleInconsistent()
{
    // TODO: stop, delay ≥2 cycles, re-arm cycle match (§6.3)
    os_log(ASLog(), "IT%u: CycleInconsistent (stub)", _ctxIndex);
}

void ASOHCIITContext::RecoverDeadContext()
{
    // Basic policy: clear run, reset counters; higher-level manager may re-init.
    ASOHCIContextBase::RecoverDeadContext();
}

