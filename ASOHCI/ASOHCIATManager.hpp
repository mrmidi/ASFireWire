//#pragma once
//
// ASOHCIATManager.hpp
// Top-level AT orchestrator: owns descriptor pool, program builder, and both contexts.
// Provides a simple API to queue packets and handles reset windows.
//
// Spec refs: OHCI 1.1 §7.6 (pipeline + reset handling), §7.5 (interrupt policy), §7.3/§7.4 (policy)

#pragma once

#include <DriverKit/IOReturn.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include "ASOHCIATDescriptorPool.hpp"
#include "ASOHCIATProgramBuilder.hpp"
#include "ASOHCIATRequestContext.hpp"
#include "ASOHCIATResponseContext.hpp"

class ASOHCIATManager {
public:
    ASOHCIATManager() = default;
    ~ASOHCIATManager() = default;
    // Bring-up: create pool, init contexts, set policies
    virtual kern_return_t Initialize(IOPCIDevice* pci, uint8_t barIndex,
                                     uint32_t poolBytes,
                                     const ATRetryPolicy& retry,
                                     const ATFairnessPolicy& fair,
                                     const ATPipelinePolicy& pipe);

    virtual kern_return_t Start();   // starts both contexts (§7.1)
    virtual kern_return_t Stop();    // stops both (§7.6)

    // Build+enqueue helpers (thin convenience on top of ProgramBuilder)
    virtual kern_return_t QueueRequest(const uint32_t* header, uint32_t headerBytes,
                                       const uint32_t* payloadPAs, const uint32_t* payloadSizes, uint32_t fragments,
                                       const ATQueueOptions& opts);

    virtual kern_return_t QueueResponse(const uint32_t* header, uint32_t headerBytes,
                                        const uint32_t* payloadPAs, const uint32_t* payloadSizes, uint32_t fragments,
                                        const ATQueueOptions& opts);

    // Interrupt fan-in from the OHCI IRQ path
    virtual void OnInterrupt_ReqTxComplete();  // §7.5
    virtual void OnInterrupt_RspTxComplete();  // §7.5

    // Bus-reset window management
    virtual void OnBusResetBegin();            // §7.6
    virtual void OnBusResetEnd();              // §7.6

    // Accessors for testing/telemetry
    virtual uint32_t OutstandingRequests() const;
    virtual uint32_t OutstandingResponses() const;

private:
    IOPCIDevice*              _pci = nullptr;
    uint8_t                   _bar = 0;

    ASOHCIATDescriptorPool    _pool;
    ASOHCIATProgramBuilder    _builderReq;
    ASOHCIATProgramBuilder    _builderRsp;

    ASOHCIATRequestContext    _req;
    ASOHCIATResponseContext   _rsp;

    ATRetryPolicy             _retry{};
    ATFairnessPolicy          _fair{};
    ATPipelinePolicy          _pipe{};
};
