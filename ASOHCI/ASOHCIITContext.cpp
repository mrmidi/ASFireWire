//
// ASOHCIITContext.cpp
// Isochronous Transmit (IT) context skeleton (DriverKit-friendly)
//
// Spec anchors:
//   Host interrupt + IsoXmit event/mask registers: OHCI 1.1 Chapter 6 (event
//   bits demux; not IT-specific semantics) IT DMA programs & descriptor usage:
//   §9.1 IT Context registers / cycle match fields: §9.2 Safe program appending
//   (tail patching semantics): §9.4 IT interrupt meanings (underrun, handling
//   late packets, cycle inconsistent): §9.5 IT data / header emission
//   (speed/tag/channel/sy, length): §9.6
//

#include "ASOHCIITContext.hpp"
#include "ASOHCICtxRegMap.hpp"
#include "ASOHCIDescriptorUtils.hpp"
#include "ASOHCIITProgramBuilder.hpp"
#include "ASOHCIMemoryBarrier.hpp"
#include "LogHelper.hpp"
#include "OHCIConstants.hpp"
#include "Shared/ASOHCIContextBase.hpp"
#include "Shared/ASOHCITypes.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOReturn.h>
#include <os/log.h>

// Memory barriers now provided by ASOHCIMemoryBarrier.hpp

// Descriptor utilities now provided by ASOHCIDescriptorUtils.hpp

// OUTPUT_* command encoding we introduced (ITDescOps)
namespace {
constexpr uint32_t kCmd_OUTPUT_MORE = 0x0;
constexpr uint32_t kCmd_OUTPUT_LAST = 0x1;
} // namespace

ASOHCIITContext::InFlightProg *ASOHCIITContext::CurrentTail() {
  if (_ringHead == _ringTail && !_ringFull)
    return nullptr;
  // Last valid element is at (ringHead - 1) modulo size if any entries exist
  uint32_t count =
      _ringFull ? kMaxInFlight
                : ((_ringHead + kMaxInFlight) - _ringTail) % kMaxInFlight;
  if (count == 0)
    return nullptr;
  uint32_t idx = (_ringHead + kMaxInFlight - 1) % kMaxInFlight;
  return &_ring[idx];
}

void ASOHCIITContext::PushProgram(const ITDesc::Program &p) {
  InFlightProg prog{p.headPA, p.tailPA, p.tailVA, p.zHead, true};
  _ring[_ringHead] = prog;
  if (_ringFull) {
    // Overwrite oldest (drop) – should not happen if completion keeps up
    _ringTail = (_ringTail + 1) % kMaxInFlight;
  }
  _ringHead = (_ringHead + 1) % kMaxInFlight;
  if (_ringHead == _ringTail)
    _ringFull = true;
}

void ASOHCIITContext::RetireOne() {
  if (_ringHead == _ringTail && !_ringFull)
    return;
  _ring[_ringTail].valid = false;
  _ringTail = (_ringTail + 1) % kMaxInFlight;
  _ringFull = false;
}

kern_return_t ASOHCIITContext::Initialize(IOPCIDevice *pci, uint8_t barIndex,
                                          uint32_t ctxIndex) {
  if (!pci)
    return kIOReturnBadArgument;
  _ctxIndex = ctxIndex;
  _policy = {};
  _last = {};

  // Compute per-context register offsets (read/base + set/clear/cmd)
  ASContextOffsets offs{};
  if (!ASOHCICtxRegMap::Compute(ASContextKind::kIT_Transmit, _ctxIndex,
                                &offs)) {
    return kIOReturnBadArgument;
  }
  return ASOHCIContextBase::Initialize(pci, barIndex,
                                       ASContextKind::kIT_Transmit, offs);
}

kern_return_t ASOHCIITContext::Start() {
  if (!_pci)
    return kIOReturnNotReady;
  // Clear run bit to ensure a clean state; do NOT program CommandPtr yet.
  WriteContextClear(kOHCI_ContextControl_run);
  // Leave CommandPtr untouched (could be 0). Real arming occurs on first
  // Enqueue.
  os_log(ASLog(), "IT%u: Start deferred (will run on first enqueue)",
         _ctxIndex);
  return kIOReturnSuccess;
}

void ASOHCIITContext::ApplyPolicy(const ITPolicy &policy) {
  _policy = policy;
  // Program cycle match using ContextControl Set/Clear (OHCI 1.1 §9.2).
  constexpr uint32_t kCycleMatchMask =
      kOHCI_IT_CycleMatchMask; // [28:16], 13 bits
  constexpr uint32_t kCycleMatchEnable =
      kOHCI_IT_CycleMatchEnable; // enable bit per §9.2
  if (!policy.cycleMatchEnable) {
    WriteContextClear(kCycleMatchEnable);
    os_log(ASLog(), "IT%u: ApplyPolicy disable cycleMatch", _ctxIndex);
  } else {
    uint32_t cycleVal = policy.startOnCycle % 8000; // 0..7999
    // Clear existing value+enable, then Set new value+enable atomically via Set
    // register.
    WriteContextClear(kCycleMatchMask | kCycleMatchEnable);
    uint32_t setVal = ((cycleVal << 16) & kCycleMatchMask) | kCycleMatchEnable;
    WriteContextSet(setVal);
    os_log(ASLog(), "IT%u: ApplyPolicy enable cycleMatch startCycle=%u",
           _ctxIndex, cycleVal);
  }

  os_log(ASLog(),
         "IT%u: ApplyPolicy dropIfLate=%u underrunBudgetUs=%u (software-only "
         "policies logged)",
         _ctxIndex, policy.dropIfLate, policy.underrunBudgetUs);
}

kern_return_t ASOHCIITContext::Enqueue(const ITDesc::Program &program,
                                       const ITQueueOptions &opts) {
  if (!_pci || !program.headPA || program.descCount == 0)
    return kIOReturnBadArgument;

  uint32_t cc = ReadContextSet();
  bool active = (cc & kOHCI_ContextControl_active) != 0;

  if (!active) {
    // Initial arm: program CommandPtr and set run (if not already) then wake
    // for immediate fetch.
    OHCI_MEMORY_BARRIER(); // Ensure all descriptor writes visible before
                           // programming CommandPtr (OHCI §9.1)
    WriteCommandPtr(program.headPA, program.zHead);
    OHCI_MEMORY_BARRIER(); // Ensure CommandPtr write is globally visible before
                           // run (OHCI §9.1)
    if ((cc & kOHCI_ContextControl_run) == 0) {
      WriteContextSet(kOHCI_ContextControl_run);
    } else {
      Wake();
    }
    _outstanding++;
    PushProgram(program);
    os_log(ASLog(), "IT%u: Enqueue initial (auto-run) head=0x%x z=%u count=%u",
           _ctxIndex, program.headPA, program.zHead, program.descCount);
    return kIOReturnSuccess;
  }

  // Active: perform safe tail append (§9.4)
  if (!opts.allowAppendWhileActive) {
    return kIOReturnBusy;
  }

  InFlightProg *tail = CurrentTail();
  if (!tail || !tail->valid || !tail->tailVA) {
    os_log(ASLog(), "IT%u: Append failed (no tracked tail)", _ctxIndex);
    return kIOReturnNotReady;
  }

  // Tail descriptor layout: quad0 (cmd/key/i), quad1 (branchAddress+Z
  // placeholder for immediate forms)
  auto *tailDesc = reinterpret_cast<ATDesc::Descriptor *>(tail->tailVA);
  uint32_t q0 = tailDesc->quad[0];
  uint32_t q1 = tailDesc->quad[1];
  (void)q1;
  // Ensure it is currently a LAST variant
  if (DescGetCmd(q0) != kCmd_OUTPUT_LAST) {
    os_log(ASLog(), "IT%u: Append tail not LAST (cmd=0x%x)", _ctxIndex,
           DescGetCmd(q0));
    return kIOReturnUnsupported;
  }

  // 1. Convert existing LAST -> MORE
  q0 = DescSetCmd(q0, kCmd_OUTPUT_MORE);
  q0 &= ~(0x3u << 10); // b=0 for *_MORE
  tailDesc->quad[0] = q0;

  // 2. Patch branchAddress
  uint32_t branch = (program.headPA & 0xFFFFFFF0u) | (program.zHead & 0xF);
  if (DescGetKey(q0) == ITDescOps::kKey_IMMEDIATE) {
    // For immediate, branch is in the second block's Q2 (skipAddress+Z)
    auto *tailDesc1 = tailDesc + 1;
    tailDesc1->quad[0] = branch;
  } else {
    tailDesc->quad[2] = branch;
  }

  OHCI_MEMORY_BARRIER(); // Ensure tail patch is visible before hardware fetches
                         // new program (OHCI §9.4)

  // 4. Convert new program final descriptor (already *_LAST from builder) — we
  // trust builder produced LAST.
  OHCI_MEMORY_BARRIER(); // Ensure all descriptor writes are visible before
                         // updating program ring (OHCI §9.4)
  _outstanding++;
  PushProgram(program);
  os_log(ASLog(), "IT%u: Append tailPA=0x%x -> newHead=0x%x branch=0x%x z=%u",
         _ctxIndex, tail->tailPA, program.headPA, branch, program.zHead);
  return kIOReturnSuccess;
}

void ASOHCIITContext::OnInterruptTx() {
  if (!_pci)
    return;
  uint32_t cc = ReadContextSet();
  InFlightProg *prog = CurrentTail(); // most recently queued still in-flight;
                                      // after retire we'll have consumed one
  uint16_t xferStatus = 0;
  uint16_t ts = 0;
  if (prog && prog->valid && prog->tailVA) {
    auto *tailDesc = reinterpret_cast<ATDesc::Descriptor *>(prog->tailVA);
    // Tentative mapping: assume controller writes completion status into quad2
    // low bits and timestamp into quad3 (implementation-specific).
    uint32_t q2 = tailDesc->quad[2];
    uint32_t q3 = tailDesc->quad[3];
    xferStatus = static_cast<uint16_t>(q2 & 0xFFFF); // low 16 bits
    ts = static_cast<uint16_t>(q3 & 0xFFFF);
    prog->lastStatus = xferStatus;
    prog->timestamp = ts;
  } else {
    // Fallback: derive a minimal status from context control if no program.
    xferStatus = static_cast<uint16_t>(cc & 0x1F);
    ts = static_cast<uint16_t>((cc >> 16) & 0xFFFF);
  }

  ASOHCIITStatus statusDec;
  _last = statusDec.Decode(xferStatus, ts);
  if (_outstanding > 0) {
    _outstanding--;
    RetireOne();
  }
  if ((_last.event == ITEvent::kUnrecoverable) ||
      (cc & kOHCI_ContextControl_dead)) {
    RecoverDeadContext();
  }
  os_log(ASLog(),
         "IT%u: Interrupt status=0x%x ts=%u success=%u event=%u outstanding=%u",
         _ctxIndex, xferStatus, ts, _last.success,
         static_cast<unsigned>(_last.event), _outstanding);
}

void ASOHCIITContext::OnCycleInconsistent() {
  if (!_pci)
    return;
  os_log(ASLog(), "IT%u: CycleInconsistent handling", _ctxIndex);
  // Stop context (clears run); outstanding packets considered lost
  WriteContextClear(kOHCI_ContextControl_run);
  _outstanding = 0;
  if (_policy.cycleMatchEnable) {
    // Read current cycle match value (bits 28-16) and increment by 2 cycles
    // (wrap 8000)
    uint32_t cc = ReadContextSet();
    uint32_t match = (cc >> 16) & 0x1FFF;
    match = (match + 2) % 8000;
    // Reprogram cycle match enable with new starting cycle
    ITPolicy p = _policy;
    p.startOnCycle = match;
    ApplyPolicy(p);
    // Restart context run
    WriteContextSet(kOHCI_ContextControl_run);
    os_log(ASLog(), "IT%u: Re-armed cycleMatch startOnCycle=%u", _ctxIndex,
           match);
  } else {
    // If no cycle match policy, just restart
    WriteContextSet(kOHCI_ContextControl_run);
  }
}

void ASOHCIITContext::RecoverDeadContext() {
  // Basic policy: clear run, reset counters; higher-level manager may re-init.
  ASOHCIContextBase::RecoverDeadContext();
}
