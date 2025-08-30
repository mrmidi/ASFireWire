// #pragma once
//
//  ASOHCIATRequestContext.hpp
//  Concrete AT Request context (uses fairness; request tCodes)
//
//  Spec refs: OHCI 1.1 §7 (all), §7.4 (Fairness applies to Request)

#pragma once

#include "ASOHCIATDescriptor.hpp"
#include "ASOHCIATPolicy.hpp"
#include "ASOHCIATTypes.hpp"
#include "Shared/ASOHCIContextBase.hpp"

class ASOHCIATRequestContext : public ASOHCIContextBase {
public:
  ASOHCIATRequestContext() = default;
  virtual ~ASOHCIATRequestContext() = default;
  // Bring-up and policy
  virtual kern_return_t Initialize(IOPCIDevice *pci, uint8_t barIndex);
  virtual void ApplyPolicy(const ATRetryPolicy &r, const ATFairnessPolicy &f,
                           const ATPipelinePolicy &p);

  // Queue one packet program (already built in pool) — no ownership transfer
  virtual kern_return_t Enqueue(const ATDesc::Program &program,
                                const ATQueueOptions &opts);

  // Interrupt hook: handle TxComplete for this context (§7.5)
  virtual void OnInterruptTxComplete();

private:
  uint32_t DrainCompletions(uint32_t maxToDrain);
};
