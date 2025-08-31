//
// ASOHCIIRManager.cpp
// Isochronous Receive (IR) manager orchestrator
//
// Spec refs (OHCI 1.1): Chapter 6 (IsoRxIntEvent/Mask demux), §10.2-10.6 (IR
// contexts),
//   §10.3 (context discovery), §10.5 (interrupt semantics), §10.6 (data
//   formats)
//

#include "ASOHCIIRManager.hpp"
#include "ASOHCICtxProbe.hpp"
#include "ASOHCIIRProgramBuilder.hpp"
#include "LogHelper.hpp"
#include "OHCIConstants.hpp"

#include <DriverKit/IOReturn.h>

kern_return_t ASOHCIIRManager::Initialize(OSSharedPtr<IOPCIDevice> pci,
                                          uint8_t barIndex,
                                          const IRPolicy &defaultPolicy) {
  if (!pci)
    return kIOReturnBadArgument;
  _pci = pci;
  _bar = barIndex;
  _defaultPolicy = defaultPolicy;

  // Probe IR contexts using similar strategy to IT
  _numCtx = ProbeContextCount();
  if (_numCtx > 32)
    _numCtx = 32;
  os_log(ASLog(),
         "IRManager: Initialize (bar=%u, dynamic allocation) contexts=%u", _bar,
         _numCtx);

  // Initialize shared descriptor pool with dynamic allocation (Linux-style)
  _pool = OSSharedPtr<ASOHCIATDescriptorPool>(new ASOHCIATDescriptorPool(),
                                              OSNoRetain);
  kern_return_t r = _pool->Initialize(_pci.get(), barIndex);
  if (r != kIOReturnSuccess) {
    os_log(ASLog(), "IRManager: pool init failed 0x%x", r);
    os_log(ASLog(), "IRManager: Continuing with degraded functionality "
                    "(following IT Manager pattern)");
    // Don't return failure - continue like IT Manager does
  } else {
    os_log(ASLog(), "IRManager: Descriptor pool initialized successfully");
  }

  // Initialize each IR context
  for (uint32_t i = 0; i < _numCtx; ++i) {
    _ctx[i] = OSSharedPtr<ASOHCIIRContext>(new ASOHCIIRContext(), OSNoRetain);
    _ctx[i]->Initialize(_pci.get(), barIndex, i);
    _ctx[i]->ApplyPolicy(_defaultPolicy);

    // Initialize context state
    _contextStates[i] = {};

    // Initialize buffer pools
    _bufferPools[i] = {};
  }

  return kIOReturnSuccess;
}

kern_return_t ASOHCIIRManager::StartAll() {
  if (!_pci)
    return kIOReturnNotReady;

  // Enable interrupt mask bits for each context present
  uint32_t mask = (_numCtx >= 32) ? 0xFFFFFFFFu : ((1u << _numCtx) - 1u);
  _pci->MemoryWrite32(_bar, kOHCI_IsoRecvIntMaskSet, mask);

  for (uint32_t i = 0; i < _numCtx; ++i) {
    _ctx[i]->Start();
  }

  os_log(ASLog(), "IRManager: StartAll enabled mask=0x%x", mask);
  return kIOReturnSuccess;
}

kern_return_t ASOHCIIRManager::StopAll() {
  if (!_pci)
    return kIOReturnNotReady;

  // Clear all interrupt mask bits
  _pci->MemoryWrite32(_bar, kOHCI_IsoRecvIntMaskClear, 0xFFFFFFFFu);

  // Stop all contexts
  for (uint32_t i = 0; i < _numCtx; ++i) {
    _ctx[i]->Stop();
    _contextStates[i].active = false;
  }

  os_log(ASLog(), "IRManager: StopAll");
  return kIOReturnSuccess;
}

kern_return_t ASOHCIIRManager::StartReception(
    uint32_t ctxId, const IRChannelFilter &channelFilter,
    const IRQueueOptions &queueOpts,
    void (*completionCallback)(const IRCompletion &, void *),
    void *callbackContext) {
  if (ctxId >= _numCtx)
    return kIOReturnBadArgument;
  if (!completionCallback)
    return kIOReturnBadArgument;

  // Configure context state
  auto &state = _contextStates[ctxId];
  state.channelFilter = channelFilter;
  state.completionCallback = completionCallback;
  state.callbackContext = callbackContext;
  state.currentMode = queueOpts.receiveMode;

  // Apply channel filter to context
  _ctx[ctxId]->ApplyChannelFilter(channelFilter);
  kern_return_t r = kIOReturnSuccess;
  if (r != kIOReturnSuccess) {
    os_log(ASLog(), "IRManager: ctx%u channel filter failed 0x%x", ctxId, r);
    return r;
  }

  // Start the context
  r = _ctx[ctxId]->Start();
  if (r != kIOReturnSuccess) {
    os_log(ASLog(), "IRManager: ctx%u start failed 0x%x", ctxId, r);
    return r;
  }

  state.active = true;
  os_log(ASLog(), "IRManager: ctx%u reception started mode=%u", ctxId,
         (uint32_t)queueOpts.receiveMode);
  return kIOReturnSuccess;
}

kern_return_t ASOHCIIRManager::StopReception(uint32_t ctxId) {
  if (ctxId >= _numCtx)
    return kIOReturnBadArgument;

  _ctx[ctxId]->Stop();
  _contextStates[ctxId].active = false;

  os_log(ASLog(), "IRManager: ctx%u reception stopped", ctxId);
  return kIOReturnSuccess;
}

kern_return_t ASOHCIIRManager::EnqueueReceiveBuffers(
    uint32_t ctxId, const void *bufferVAs[], const uint32_t bufferPAs[],
    const uint32_t bufferSizes[], uint32_t bufferCount,
    const IRQueueOptions &opts) {
  if (ctxId >= _numCtx)
    return kIOReturnBadArgument;
  if (!bufferVAs || !bufferPAs || !bufferSizes || bufferCount == 0)
    return kIOReturnBadArgument;
  if (!_pool->IsInitialized())
    return kIOReturnNotReady;

  auto &state = _contextStates[ctxId];
  if (!state.active)
    return kIOReturnNotReady;

  // Build receive program for the specified mode
  IRDesc::Program program;
  kern_return_t r =
      BuildStandardProgram(opts.receiveMode, bufferVAs, bufferPAs, bufferSizes,
                           bufferCount, opts, &program);
  if (r != kIOReturnSuccess) {
    return r;
  }

  // Enqueue the program to the context
  r = _ctx[ctxId]->EnqueueStandard(program, opts);
  if (r != kIOReturnSuccess) {
    os_log(ASLog(), "IRManager: ctx%u enqueue failed 0x%x", ctxId, r);
    return r;
  }

  // Store buffers in pool for management
  auto &pool = _bufferPools[ctxId];
  for (uint32_t i = 0; i < bufferCount && pool.bufferCount < 64; ++i) {
    uint32_t idx = pool.bufferCount++;
    pool.bufferVAs[idx] = const_cast<void *>(bufferVAs[i]);
    pool.bufferPAs[idx] = bufferPAs[i];
    pool.bufferSizes[idx] = bufferSizes[i];
    pool.bufferInUse[idx] = true;
  }

  os_log(ASLog(), "IRManager: ctx%u enqueued %u buffers mode=%u", ctxId,
         bufferCount, (uint32_t)opts.receiveMode);
  return kIOReturnSuccess;
}

kern_return_t ASOHCIIRManager::EnqueueDualBufferReceive(
    uint32_t ctxId, const IRDualBufferInfo &dualBufferInfo,
    const IRQueueOptions &opts) {
  if (ctxId >= _numCtx)
    return kIOReturnBadArgument;
  if (!_pool->IsInitialized())
    return kIOReturnNotReady;

  auto &state = _contextStates[ctxId];
  if (!state.active)
    return kIOReturnNotReady;
  if (opts.receiveMode != IRMode::kDualBuffer)
    return kIOReturnBadArgument;

  // Build dual-buffer program
  IRProgram::DualBufferProgram program;
  kern_return_t r = BuildDualBufferProgram(dualBufferInfo, opts, &program);
  if (r != kIOReturnSuccess) {
    return r;
  }

  // Enqueue to context (would need additional method in IRContext)
  os_log(ASLog(), "IRManager: ctx%u dual-buffer enqueue (stub)", ctxId);
  return kIOReturnUnsupported; // Implementation pending
}

void ASOHCIIRManager::OnInterrupt_RxEventMask(uint32_t mask) {
  uint32_t remaining = mask;
  while (remaining) {
    uint32_t bit = __builtin_ctz(remaining);
    if (bit < _numCtx && _contextStates[bit].active) {
      // Route interrupt to the context's existing interrupt handler
      _ctx[bit]->OnInterruptRx();
    }
    remaining &= ~(1u << bit);
  }
}

void ASOHCIIRManager::OnInterrupt_BusReset() {
  // Stop all active contexts on bus reset
  for (uint32_t i = 0; i < _numCtx; ++i) {
    if (_contextStates[i].active) {
      _ctx[i]->OnBusReset();
    }
  }
  os_log(ASLog(), "IRManager: bus reset handled for %u contexts", _numCtx);
}

kern_return_t ASOHCIIRManager::RefillContext(uint32_t ctxId) {
  if (ctxId >= _numCtx)
    return kIOReturnBadArgument;

  auto &pool = _bufferPools[ctxId];
  if (pool.bufferCount == 0)
    return kIOReturnNoResources;

  // Find available buffers and re-enqueue them
  uint32_t availableCount = 0;
  for (uint32_t i = 0; i < pool.bufferCount; ++i) {
    if (!pool.bufferInUse[i]) {
      availableCount++;
    }
  }

  if (availableCount == 0)
    return kIOReturnNoResources;

  // Re-enqueue available buffers (simplified)
  os_log(ASLog(), "IRManager: ctx%u refill with %u buffers", ctxId,
         availableCount);
  return kIOReturnSuccess;
}

bool ASOHCIIRManager::ContextNeedsRefill(uint32_t ctxId) const {
  if (ctxId >= _numCtx)
    return false;

  // Check if context is running low on buffers
  return _ctx[ctxId]->NeedsRefill();
}

const IRStats &ASOHCIIRManager::GetContextStats(uint32_t ctxId) const {
  if (ctxId >= _numCtx) {
    static IRStats emptyStats{};
    return emptyStats;
  }
  return _ctx[ctxId]->GetStats();
}

void ASOHCIIRManager::ResetContextStats(uint32_t ctxId) {
  if (ctxId < _numCtx) {
    _ctx[ctxId]->ResetStats();
  }
}

uint32_t ASOHCIIRManager::ProbeContextCount() {
  if (!_pci)
    return 0;

  // Strategy: write all-ones to mask set, read back to see which bits hardware
  // implements
  _pci->MemoryWrite32(_bar, kOHCI_IsoRecvIntMaskSet, 0xFFFFFFFFu);
  uint32_t mask = 0;
  _pci->MemoryRead32(_bar, kOHCI_IsoRecvIntMaskSet, &mask);

  // Clear any unintended enables
  _pci->MemoryWrite32(_bar, kOHCI_IsoRecvIntMaskClear, 0xFFFFFFFFu);

  if (mask == 0)
    return 0;

  // Count contiguous low-order bits until first zero
  uint32_t count = 0;
  while (mask & 0x1) {
    count++;
    mask >>= 1;
  }

  os_log(ASLog(), "IRManager: probed %u IR contexts", count);
  return count;
}

kern_return_t ASOHCIIRManager::BuildStandardProgram(
    IRMode mode, const void *bufferVAs[], const uint32_t bufferPAs[],
    const uint32_t bufferSizes[], uint32_t bufferCount,
    const IRQueueOptions &opts, IRDesc::Program *outProgram) {
  if (!outProgram)
    return kIOReturnBadArgument;

  // Use program builder to construct descriptor chain
  ASOHCIIRProgramBuilder builder;
  builder.Begin(*_pool, bufferCount + 1); // buffers + LAST descriptor

  kern_return_t r = kIOReturnSuccess;

  switch (mode) {
  case IRMode::kBufferFill:
    // Use first buffer for buffer-fill mode
    r = builder.BuildBufferFillProgram(bufferPAs[0], bufferSizes[0], opts,
                                       outProgram);
    break;

  case IRMode::kPacketPerBuffer:
    r = builder.BuildPacketPerBufferProgram(bufferPAs, bufferSizes, bufferCount,
                                            opts, outProgram);
    break;

  default:
    return kIOReturnBadArgument;
  }

  return r;
}

kern_return_t ASOHCIIRManager::BuildDualBufferProgram(
    const IRDualBufferInfo &info, const IRQueueOptions &opts,
    IRProgram::DualBufferProgram *outProgram) {
  if (!outProgram)
    return kIOReturnBadArgument;

  // Build dual-buffer program using OHCI DUALBUFFER descriptors
  ASOHCIIRProgramBuilder builder;
  builder.Begin(*_pool, 3); // descriptor count estimate

  kern_return_t r = builder.BuildDualBufferProgram(info, 1, opts, outProgram);
  return r;
}