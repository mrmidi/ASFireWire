#pragma once

#include "IAsyncSubsystemPort.hpp"

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOReturn.h>
#endif

namespace ASFW::Shared {
class DMAMemoryManager;
}

namespace ASFW::Async {

struct AsyncBusStateSnapshot {
    uint16_t generation16{0};
    uint8_t generation8{0};
    uint16_t localNodeID{0};
};

class IAsyncControllerPort : public IAsyncSubsystemPort {
  public:
    ~IAsyncControllerPort() override = default;

    /**
     * @brief Arm receive-side async contexts after the controller is staged.
     *
     * This is the controller-facing bring-up hook used during hardware start.
     */
    virtual kern_return_t ArmARContextsOnly() = 0;

    /// Schedule work onto the async workloop without inline re-entrancy.
    virtual void PostToWorkloop(void (^block)()) = 0;

    /// Notify the async engine that AT completion interrupts were observed.
    virtual void OnTxInterrupt() = 0;

    /// Notify the async engine that an AR request packet is ready.
    virtual void OnRxRequestInterrupt() = 0;

    /// Notify the async engine that an AR response packet is ready.
    virtual void OnRxResponseInterrupt() = 0;

    /// Begin bus-reset teardown for the upcoming generation.
    virtual void OnBusResetBegin(uint8_t nextGen) = 0;

    /// Finish bus-reset recovery once the hardware reports a stable generation.
    virtual void OnBusResetComplete(uint8_t stableGen) = 0;

    /// Confirm the final bus generation after topology parsing completes.
    virtual void ConfirmBusGeneration(uint8_t confirmedGeneration) = 0;

    /// Stop AT contexts while preserving the rest of the async engine state.
    virtual void StopATContextsOnly() = 0;

    /// Drain any AT completion state before contexts are re-armed.
    virtual void FlushATContexts() = 0;

    /// Re-arm stopped AT contexts after bus-reset recovery.
    virtual void RearmATContexts() = 0;

    /// Snapshot the controller-visible async bus state.
    [[nodiscard]] virtual AsyncBusStateSnapshot GetBusStateSnapshot() const = 0;

    /// Expose DMA memory services needed by controller-side facades.
    [[nodiscard]] virtual Shared::DMAMemoryManager* GetDMAManager() = 0;
};

} // namespace ASFW::Async
