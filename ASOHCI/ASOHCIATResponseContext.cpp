//
// ASOHCIATResponseContext.cpp
// ASOHCI  
//
// OHCI 1.1 AT Response Context Implementation
// Based on OHCI 1.1 Specification §7.5 (Responses follow IEEE1394a rules, not fairness)
//

#include "ASOHCIATResponseContext.hpp"
#include "OHCIConstants.hpp"
#include "LogHelper.hpp"
#include <DriverKit/IOLib.h>

kern_return_t ASOHCIATResponseContext::Initialize(IOPCIDevice* pci, uint8_t barIndex)
{
    ASContextOffsets offs{};
    offs.contextBase        = kOHCI_AsRspTrContextBase;
    offs.contextControlSet  = kOHCI_AsRspTrContextControlS;
    offs.contextControlClear= kOHCI_AsRspTrContextControlC;
    offs.commandPtr         = kOHCI_AsRspTrCommandPtr;
    return ASOHCIContextBase::Initialize(pci, barIndex, ASContextKind::kAT_Response, offs);
}

void ASOHCIATResponseContext::ApplyPolicy(const ATRetryPolicy& retry, const ATFairnessPolicy& fair, const ATPipelinePolicy& pipe)
{
    // Track outstanding capacity
    if (pipe.allowPipelining) {
        _outstandingCap = pipe.maxOutstanding;
    } else {
        _outstandingCap = 1;
    }

    if (!_pci) return;

    // Program ATRetries per OHCI §5.4 — caller provides raw policy bits
    _pci->MemoryWrite32(_bar, kOHCI_ATRetries, retry.raw);

    (void)fair; // fairness not applied for responses (no special knobs here)
}

kern_return_t ASOHCIATResponseContext::Enqueue(const ATDesc::Program& program, const ATQueueOptions& opts)
{
    if (!_pci || !program.headPA || program.descCount == 0) {
        return kIOReturnBadArgument;
    }
    if (_outstanding >= _outstandingCap) {
        return kIOReturnNoSpace;
    }

    uint32_t cc = ReadContextSet();
    bool wasActive = (cc & kOHCI_ContextControl_active) != 0;
    if (!wasActive) {
        #ifndef OSMemoryFence
        #define OSMemoryFence() __sync_synchronize()
        #endif
        OSMemoryFence();
        WriteCommandPtr(program.headPA, program.zHead);
        if (cc & kOHCI_ContextControl_run) {
            Wake();
        }
    } else {
        Wake();
        IOSleep(1);
    cc = ReadContextSet();
        if ((cc & kOHCI_ContextControl_active) == 0) {
            OSMemoryFence();
            WriteCommandPtr(program.headPA, program.zHead);
            Wake();
        } else {
            return kIOReturnBusy;
        }
    }
    _outstanding++;
    (void)opts;
    return kIOReturnSuccess;
}

void ASOHCIATResponseContext::OnInterruptTxComplete()
{
    if (!_pci) return;
    uint32_t completed = DrainCompletions(16);
    if (completed > 0) {
        if (_outstanding >= completed) _outstanding -= completed; else _outstanding = 0;
    }
    uint32_t cc2 = ReadContextSet();
    if (cc2 & kOHCI_ContextControl_dead) {
        RecoverDeadContext();
    }
}

uint32_t ASOHCIATResponseContext::DrainCompletions(uint32_t maxToDrain)
{
    uint32_t completed = 0;
    uint32_t cc = ReadContextSet();
    uint32_t eventCode = cc & 0x1F;
    bool hasCompletion = false;
    switch (eventCode) {
        case 0x11: case 0x12: case 0x14: case 0x15: case 0x16:
        case 0x1B: case 0x1D: case 0x1E: case 0x03: case 0x04:
        case 0x0A: case 0x0F:
            hasCompletion = true; break;
        default:
            break;
    }
    if (hasCompletion && completed < maxToDrain) completed = 1;
    return completed;
}
