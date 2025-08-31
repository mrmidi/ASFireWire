//
// ASOHCIATManager.cpp
// ASOHCI
//
// OHCI 1.1 AT Manager Implementation
// Based on OHCI 1.1 Specification §7.4 (AT Retries), §7.5 (Fairness), §7.7 (AT
// Pipelining)
//

#include "ASOHCIATManager.hpp"
#include "LogHelper.hpp"
#include "OHCIConstants.hpp"

kern_return_t ASOHCIATManager::Initialize(OSSharedPtr<IOPCIDevice> pci,
                                          uint8_t barIndex,
                                          const ATRetryPolicy &retry,
                                          const ATFairnessPolicy &fair,
                                          const ATPipelinePolicy &pipe) {
  if (!pci) {
    os_log(ASLog(), "ASOHCIATManager: Invalid PCI device");
    return kIOReturnBadArgument;
  }

  _pci = pci;
  _bar = barIndex;
  _retry = retry;
  _fair = fair;
  _pipe = pipe;

  // Create components with std::unique_ptr
  _pool = std::make_unique<ASOHCIATDescriptorPool>();
  _builderReq = std::make_unique<ASOHCIATProgramBuilder>();
  _builderRsp = std::make_unique<ASOHCIATProgramBuilder>();
  _req = std::make_unique<ASOHCIATRequestContext>();
  _rsp = std::make_unique<ASOHCIATResponseContext>();

  kern_return_t result;

  // Initialize descriptor pool with dynamic allocation (Linux-style)
  result = _pool->Initialize(_pci.get(), barIndex);
  if (result != kIOReturnSuccess) {
    os_log(ASLog(),
           "ASOHCIATManager: Failed to initialize descriptor pool: 0x%x",
           result);
    os_log(ASLog(), "ASOHCIATManager: Continuing with degraded functionality "
                    "(following IT Manager pattern)");
    // Don't return failure - continue like IT Manager does
  } else {
    os_log(ASLog(),
           "ASOHCIATManager: Descriptor pool initialized successfully");
  }

  // Initialize AT Request context
  result = _req->Initialize(_pci.get(), barIndex);
  if (result != kIOReturnSuccess) {
    os_log(ASLog(),
           "ASOHCIATManager: Failed to initialize Request context: 0x%x",
           result);
    return result;
  }

  // Initialize AT Response context
  result = _rsp->Initialize(_pci.get(), barIndex);
  if (result != kIOReturnSuccess) {
    os_log(ASLog(),
           "ASOHCIATManager: Failed to initialize Response context: 0x%x",
           result);
    return result;
  }

  // Apply policies to both contexts
  _req->ApplyPolicy(retry, fair, pipe);
  _rsp->ApplyPolicy(retry, fair, pipe);

  os_log(ASLog(),
         "ASOHCIATManager: Initialized with dynamic allocation, pipelining=%s, "
         "maxOutstanding=%u",
         pipe.allowPipelining ? "enabled" : "disabled", pipe.maxOutstanding);

  return kIOReturnSuccess;
}

kern_return_t ASOHCIATManager::Start() {
  if (!_pci) {
    return kIOReturnNotReady;
  }

  kern_return_t result;

  // Start AT Request context first
  result = _req->Start();
  if (result != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCIATManager: Failed to start Request context: 0x%x",
           result);
    return result;
  }

  // Start AT Response context
  result = _rsp->Start();
  if (result != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCIATManager: Failed to start Response context: 0x%x",
           result);
    _req->Stop(); // Clean up Request context
    return result;
  }

  os_log(ASLog(), "ASOHCIATManager: Started both AT contexts");
  return kIOReturnSuccess;
}

kern_return_t ASOHCIATManager::Stop() {
  kern_return_t reqResult = kIOReturnSuccess;
  kern_return_t rspResult = kIOReturnSuccess;

  if (_pci) {
    // Stop both contexts (order doesn't matter for stop)
    reqResult = _req->Stop();
    if (reqResult != kIOReturnSuccess) {
      os_log(ASLog(), "ASOHCIATManager: Failed to stop Request context: 0x%x",
             reqResult);
    }

    rspResult = _rsp->Stop();
    if (rspResult != kIOReturnSuccess) {
      os_log(ASLog(), "ASOHCIATManager: Failed to stop Response context: 0x%x",
             rspResult);
    }

    if (reqResult == kIOReturnSuccess && rspResult == kIOReturnSuccess) {
      os_log(ASLog(), "ASOHCIATManager: Stopped both AT contexts");
    }
  }

  // Return first error encountered, or success
  return (reqResult != kIOReturnSuccess) ? reqResult : rspResult;
}

kern_return_t ASOHCIATManager::QueueRequest(const uint32_t *header,
                                            uint32_t headerBytes,
                                            const uint32_t *payloadPAs,
                                            const uint32_t *payloadSizes,
                                            uint32_t fragments,
                                            const ATQueueOptions &opts) {
  if (!_pci || !header) {
    return kIOReturnBadArgument;
  }

  // Validate header size per OHCI §7.1
  if (headerBytes != 8 && headerBytes != 12 && headerBytes != 16) {
    os_log(ASLog(), "ASOHCIATManager: Invalid header size %u for request",
           headerBytes);
    return kIOReturnBadArgument;
  }

  // Calculate required descriptor count: 1 header + fragments + 1 last
  uint32_t maxDescriptors = 1 + fragments + 1;
  if (maxDescriptors > 7) {
    os_log(ASLog(), "ASOHCIATManager: Too many fragments %u (max 5)",
           fragments);
    return kIOReturnBadArgument;
  }

  // Build program using Request builder
  _builderReq->Begin(*_pool, maxDescriptors);

  // Add header as immediate data
  _builderReq->AddHeaderImmediate(header, headerBytes, opts.interruptPolicy);

  // Add payload fragments
  for (uint32_t i = 0; i < fragments; i++) {
    if (payloadPAs && payloadSizes && payloadSizes[i] > 0) {
      _builderReq->AddPayloadFragment(payloadPAs[i], payloadSizes[i]);
    }
  }

  // Finalize program
  ATDesc::Program program = _builderReq->Finalize();
  if (!program.headPA) {
    os_log(ASLog(), "ASOHCIATManager: Failed to build request program");
    _builderReq->Cancel();
    return kIOReturnNoMemory;
  }

  // Enqueue to Request context
  kern_return_t result = _req->Enqueue(program, opts);
  if (result != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCIATManager: Failed to enqueue request: 0x%x", result);
    return result;
  }

  os_log(ASLog(), "ASOHCIATManager: Queued request with %u fragments",
         fragments);
  return kIOReturnSuccess;
}

kern_return_t ASOHCIATManager::QueueResponse(const uint32_t *header,
                                             uint32_t headerBytes,
                                             const uint32_t *payloadPAs,
                                             const uint32_t *payloadSizes,
                                             uint32_t fragments,
                                             const ATQueueOptions &opts) {
  if (!_pci || !header) {
    return kIOReturnBadArgument;
  }

  // Validate header size per OHCI §7.1
  if (headerBytes != 8 && headerBytes != 12 && headerBytes != 16) {
    os_log(ASLog(), "ASOHCIATManager: Invalid header size %u for response",
           headerBytes);
    return kIOReturnBadArgument;
  }

  // Calculate required descriptor count
  uint32_t maxDescriptors = 1 + fragments + 1;
  if (maxDescriptors > 7) {
    os_log(ASLog(), "ASOHCIATManager: Too many fragments %u (max 5)",
           fragments);
    return kIOReturnBadArgument;
  }

  // Build program using Response builder
  _builderRsp->Begin(*_pool, maxDescriptors);

  // Add header as immediate data (responses may include timestamp)
  _builderRsp->AddHeaderImmediate(header, headerBytes, opts.interruptPolicy);

  // Add payload fragments
  for (uint32_t i = 0; i < fragments; i++) {
    if (payloadPAs && payloadSizes && payloadSizes[i] > 0) {
      _builderRsp->AddPayloadFragment(payloadPAs[i], payloadSizes[i]);
    }
  }

  // Finalize program
  ATDesc::Program program = _builderRsp->Finalize();
  if (!program.headPA) {
    os_log(ASLog(), "ASOHCIATManager: Failed to build response program");
    _builderRsp->Cancel();
    return kIOReturnNoMemory;
  }

  // Enqueue to Response context
  kern_return_t result = _rsp->Enqueue(program, opts);
  if (result != kIOReturnSuccess) {
    os_log(ASLog(), "ASOHCIATManager: Failed to enqueue response: 0x%x",
           result);
    return result;
  }

  os_log(ASLog(), "ASOHCIATManager: Queued response with %u fragments",
         fragments);
  return kIOReturnSuccess;
}

void ASOHCIATManager::OnInterrupt_ReqTxComplete() {
  // Fan-out interrupt to Request context per OHCI §7.6
  _req->OnInterruptTxComplete();

  os_log(ASLog(), "ASOHCIATManager: Processed reqTxComplete interrupt");
}

void ASOHCIATManager::OnInterrupt_RspTxComplete() {
  // Fan-out interrupt to Response context per OHCI §7.6
  _rsp->OnInterruptTxComplete();

  os_log(ASLog(), "ASOHCIATManager: Processed respTxComplete interrupt");
}

void ASOHCIATManager::OnBusResetBegin() {
  // Per OHCI §7.2.3.1: AT contexts cease transmission on bus reset
  // Fan-out to both contexts
  _req->OnBusResetBegin();
  _rsp->OnBusResetBegin();

  os_log(ASLog(),
         "ASOHCIATManager: Bus reset begin - stopping AT transmission");
}

void ASOHCIATManager::OnBusResetEnd() {
  // Per OHCI §7.2.3.2: Wait for contexts to quiesce before clearing busReset
  _req->OnBusResetEnd();
  _rsp->OnBusResetEnd();

  os_log(ASLog(),
         "ASOHCIATManager: Bus reset end - AT contexts ready for restart");

  // Note: Software must ensure NodeID.iDValid is set and nodeNumber != 63
  // before restarting contexts after bus reset (per OHCI §7.2.3.2)
}

uint32_t ASOHCIATManager::OutstandingRequests() const {
  // For telemetry - would need to expose this from ASOHCIATContextBase
  // Simplified implementation
  return _req->IsActive() ? 1 : 0;
}

uint32_t ASOHCIATManager::OutstandingResponses() const {
  // For telemetry - would need to expose this from ASOHCIATContextBase
  // Simplified implementation
  return _rsp->IsActive() ? 1 : 0;
}
