//#pragma once
//
// ASOHCIATResponseContext.hpp
// Concrete AT Response context (response tCodes; fairness not applied)
//
// Spec refs: OHCI 1.1 §7 (all), §7.4 (responses follow 1394a response rules)

#pragma once

#include "Shared/ASOHCIContextBase.hpp"
#include <DriverKit/DriverKit.h>
#include "ASOHCIATPolicy.hpp"
#include "ASOHCIATTypes.hpp"
#include "ASOHCIATDescriptor.hpp"

class ASOHCIATResponseContext : public ASOHCIContextBase {
public:
    ASOHCIATResponseContext() = default;
    virtual ~ASOHCIATResponseContext() = default;
    // Bring-up and policy (responses don't use fairness)
    virtual kern_return_t Initialize(IOPCIDevice* pci, uint8_t barIndex);
    virtual void          ApplyPolicy(const ATRetryPolicy& r, const ATFairnessPolicy& f, const ATPipelinePolicy& p);

    // Queue and IRQ handling mirror Request for now
    virtual kern_return_t Enqueue(const ATDesc::Program& program, const ATQueueOptions& opts);
    virtual void          OnInterruptTxComplete();

private:
    uint32_t DrainCompletions(uint32_t maxToDrain);
};
