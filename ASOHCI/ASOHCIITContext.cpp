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
    // Program cycle match (OHCI 1.1 §9.2) if requested. Cycle match fields live in the context control register.
    // Sequence: if disabling, clear enable bit; if enabling, write cycle start + enable atomically via Set.
    volatile uint32_t* ctrlSet = _mmio32(_offsets.contextControlSet);
    volatile uint32_t* ctrlClr = _mmio32(_offsets.contextControlClear);

    // Bits layout (subset) for IsoXmit ContextControl (refer §9.2):
    //  31: Run; 30: Reserved; 29: CycleMatchEnable; 28-16: CycleMatch; others: interrupt/status bits.
    constexpr uint32_t kCycleMatchEnableBit = 1u << 29; // spec symbolic mapping
    constexpr uint32_t kCycleMatchMask      = 0x1FFFu << 16; // 13 bits of cycle value (0..7999)

    if (!policy.cycleMatchEnable) {
        // Clear enable; do not disturb current cycle value.
        *ctrlClr = kCycleMatchEnableBit;
        os_log(ASLog(), "IT%u: ApplyPolicy disable cycleMatch", _ctxIndex);
    } else {
        uint32_t cycleVal = policy.startOnCycle % 8000; // wrap to 0..7999 (OHCI cycle range)
        // Clear prior cycle match bits then set new value + enable.
        *ctrlClr = kCycleMatchEnableBit | kCycleMatchMask; // clear enable + previous value
        uint32_t setVal = ((cycleVal << 16) & kCycleMatchMask) | kCycleMatchEnableBit;
        *ctrlSet = setVal;
        os_log(ASLog(), "IT%u: ApplyPolicy enable cycleMatch startCycle=%u", _ctxIndex, cycleVal);
    }

    os_log(ASLog(), "IT%u: ApplyPolicy dropIfLate=%u underrunBudgetUs=%u (software-only policies logged)",
           _ctxIndex, policy.dropIfLate, policy.underrunBudgetUs);
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

