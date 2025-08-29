//
// ASOHCIITContext.cpp
// Isochronous Transmit (IT) context skeleton (DriverKit-friendly)
//
// Spec anchors:
//   Host interrupt + IsoXmit event/mask registers: OHCI 1.1 Chapter 6 (event bits demux; not IT-specific semantics)
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
    if (!_pci || !program.headPA || program.descCount == 0) return kIOReturnBadArgument;

    uint32_t cc = ReadContextControlCached();
    bool active = (cc & kOHCI_ContextControl_active) != 0;

    if (!active) {
        // Initial arm: memory fence then write CommandPtr + wake if already running
        #ifndef OSMemoryFence
        #define OSMemoryFence() __sync_synchronize()
        #endif
        OSMemoryFence();
        WriteCommandPtr(program.headPA, program.zHead);
        if (cc & kOHCI_ContextControl_run) {
            Wake();
        }
        _outstanding++;
        os_log_debug(ASLog(), "IT%u: Enqueue initial head=0x%x z=%u count=%u", _ctxIndex, program.headPA, program.zHead, program.descCount);
        return kIOReturnSuccess;
    }

    // Active: attempt safe tail append placeholder. Real implementation must patch OUTPUT_LAST per §9.4.
    if (!opts.allowAppendWhileActive) {
        return kIOReturnBusy;
    }
    // For now we do not support live tail patching; indicate unsupported.
    os_log_debug(ASLog(), "IT%u: Enqueue append unsupported (active context)", _ctxIndex);
    return kIOReturnUnsupported;
}

void ASOHCIITContext::OnInterruptTx()
{
    if (!_pci) return;
    // Read context control for event/status bits (placeholder: low bits hold xferStatus analog)
    uint32_t cc = ReadContextControlCached();
    uint16_t xferStatus = static_cast<uint16_t>(cc & 0x1F); // heuristic
    uint16_t ts = static_cast<uint16_t>((cc >> 16) & 0xFFFF); // assume timestamp in upper half (placeholder)
    ASOHCIITStatus statusDec;
    _last = statusDec.Decode(xferStatus, ts);
    if (_outstanding > 0) {
        // For now treat any interrupt as completion of one packet until real descriptor readback added
        _outstanding--;
    }
    if ((_last.event == ITEvent::kUnrecoverable) || (cc & kOHCI_ContextControl_dead)) {
        RecoverDeadContext();
    }
    os_log_debug(ASLog(), "IT%u: Interrupt xferStatus=0x%x ts=%u success=%u event=%u outstanding=%u", _ctxIndex,
                 xferStatus, ts, _last.success, static_cast<unsigned>(_last.event), _outstanding);
}

void ASOHCIITContext::OnCycleInconsistent()
{
    if (!_pci) return;
    os_log(ASLog(), "IT%u: CycleInconsistent handling", _ctxIndex);
    // Stop context (clears run); outstanding packets considered lost
    WriteContextClear(kOHCI_ContextControl_run);
    _outstanding = 0;
    if (_policy.cycleMatchEnable) {
        // Read current cycle match value (bits 28-16) and increment by 2 cycles (wrap 8000)
        uint32_t cc = ReadContextControlCached();
        uint32_t match = (cc >> 16) & 0x1FFF;
        match = (match + 2) % 8000;
        // Reprogram cycle match enable with new starting cycle
        ITPolicy p = _policy;
        p.startOnCycle = match;
        ApplyPolicy(p);
        // Restart context run
        WriteContextSet(kOHCI_ContextControl_run);
        os_log(ASLog(), "IT%u: Re-armed cycleMatch startOnCycle=%u", _ctxIndex, match);
    } else {
        // If no cycle match policy, just restart
        WriteContextSet(kOHCI_ContextControl_run);
    }
}

void ASOHCIITContext::RecoverDeadContext()
{
    // Basic policy: clear run, reset counters; higher-level manager may re-init.
    ASOHCIContextBase::RecoverDeadContext();
}

