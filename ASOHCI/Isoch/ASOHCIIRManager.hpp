#pragma once
//
// ASOHCIIRManager.hpp
// Owns multiple IR contexts, descriptor pool sharing, and interrupt fan-out.
//
// Spec refs (OHCI 1.1): Chapter 6 (IsoRxIntEvent demux), §10.3 (context
// discovery),
//   §10.2 (receive modes), §10.5 (interrupt semantics), §10.6 (data formats)

#include "../Async/ASOHCIATDescriptorPool.hpp"
#include "ASOHCIIRContext.hpp"
#include "ASOHCIIRDescriptor.hpp"
#include "ASOHCIIRTypes.hpp"
#include <DriverKit/OSSharedPtr.h>
#include <memory>

class ASOHCIIRManager {
public:
  ASOHCIIRManager() = default;
  ~ASOHCIIRManager() = default;

  // Discover available IR contexts, init shared pool, apply defaults.
  virtual kern_return_t Initialize(OSSharedPtr<IOPCIDevice> pci,
                                   uint8_t barIndex,
                                   const IRPolicy &defaultPolicy);

  virtual kern_return_t StartAll();
  virtual kern_return_t StopAll();

  // Configure and start reception on a specific IR context
  virtual kern_return_t
  StartReception(uint32_t ctxId, const IRChannelFilter &channelFilter,
                 const IRQueueOptions &queueOpts,
                 void (*completionCallback)(const IRCompletion &, void *),
                 void *callbackContext);

  // Stop reception on a specific context
  virtual kern_return_t StopReception(uint32_t ctxId);

  // Enqueue receive buffers for standard modes (buffer-fill, packet-per-buffer)
  virtual kern_return_t EnqueueReceiveBuffers(uint32_t ctxId,
                                              const void *bufferVAs[],
                                              const uint32_t bufferPAs[],
                                              const uint32_t bufferSizes[],
                                              uint32_t bufferCount,
                                              const IRQueueOptions &opts);

  // Enqueue dual-buffer reception
  virtual kern_return_t
  EnqueueDualBufferReceive(uint32_t ctxId,
                           const IRDualBufferInfo &dualBufferInfo,
                           const IRQueueOptions &opts);

  // Top-half: called from the device's main ISR after reading host IntEvent
  // (§6.4)
  virtual void OnInterrupt_RxEventMask(uint32_t mask);
  virtual void OnInterrupt_BusReset(); // Clear context state on bus reset

  // Buffer management
  virtual kern_return_t RefillContext(uint32_t ctxId);
  virtual bool ContextNeedsRefill(uint32_t ctxId) const;

  // Statistics and monitoring
  virtual const IRStats &GetContextStats(uint32_t ctxId) const;
  virtual void ResetContextStats(uint32_t ctxId);

  // Context discovery
  uint32_t NumContexts() const { return _numCtx; }
  bool IsContextValid(uint32_t ctxId) const { return ctxId < _numCtx; }

protected:
  // Probe isoRxIntMask to figure out how many IR contexts exist (§6.4)
  virtual uint32_t ProbeContextCount();

  // Create receive program builders for different modes
  virtual kern_return_t
  BuildStandardProgram(IRMode mode, const void *bufferVAs[],
                       const uint32_t bufferPAs[], const uint32_t bufferSizes[],
                       uint32_t bufferCount, const IRQueueOptions &opts,
                       IRDesc::Program *outProgram);

  virtual kern_return_t
  BuildDualBufferProgram(const IRDualBufferInfo &info,
                         const IRQueueOptions &opts,
                         IRProgram::DualBufferProgram *outProgram);

private:
  OSSharedPtr<IOPCIDevice> _pci;
  uint8_t _bar = 0;

  std::unique_ptr<ASOHCIIRContext>
      _ctx[32]; // upper bound; actual count discovered
  uint32_t _numCtx = 0;

  std::unique_ptr<ASOHCIATDescriptorPool> _pool; // shared pool reused from AT
  IRPolicy _defaultPolicy{};

  // Context state tracking
  struct ContextState {
    bool active = false;
    IRMode currentMode = IRMode::kPacketPerBuffer;
    IRChannelFilter channelFilter{};
    void (*completionCallback)(const IRCompletion &, void *) = nullptr;
    void *callbackContext = nullptr;
  };
  ContextState _contextStates[32];

  // Buffer pool management for automatic refill
  struct BufferPool {
    void *bufferVAs[64];
    uint32_t bufferPAs[64];
    uint32_t bufferSizes[64];
    bool bufferInUse[64];
    uint32_t bufferCount = 0;
  };
  BufferPool _bufferPools[32]; // One pool per context
};