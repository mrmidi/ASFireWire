#pragma once

#include "IRMClient.hpp"
#include "IRMTypes.hpp"
#include <functional>
#include <optional>

namespace ASFW::IRM {

/**
 * Callback invoked when allocation is lost after bus reset and cannot be recovered.
 *
 * @param channel Channel that was lost (0xFF = no channel)
 * @param bandwidthUnits Bandwidth units that were lost
 *
 * Example:
 *   auto lostCallback = [](uint8_t channel, uint32_t bandwidth) {
 *       os_log_error(OS_LOG_DEFAULT,
 *                    "IRM allocation lost: channel %u, %u bandwidth units",
 *                    channel, bandwidth);
 *       // Stop streaming, notify user, etc.
 *   };
 */
using AllocationLostCallback = std::function<void(uint8_t channel, uint32_t bandwidthUnits)>;

/**
 * IRMAllocationManager - Manages IRM allocations with automatic bus reset recovery.
 *
 * This implements Phase 2 (Bus Reset Recovery) from IRM-Implementation-Plan.md.
 *
 * Design Philosophy:
 * - Tracks active allocation (channel + bandwidth + generation)
 * - Automatically attempts to reallocate after bus reset
 * - Notifies client if reallocation fails
 * - Single active allocation per manager instance
 *
 * Behavior:
 * 1. Client calls Allocate(channel, bandwidth)
 * 2. Manager calls IRMClient to allocate resources
 * 3. On success, records allocation and generation
 * 4. When bus reset occurs, OnBusReset(newGeneration) is called
 * 5. Manager automatically tries to reallocate same resources
 * 6. If reallocation succeeds, operation continues transparently
 * 7. If reallocation fails, AllocationLostCallback is invoked
 *
 * Usage Example:
 *   IRMAllocationManager allocMgr(irmClient);
 *
 *   // Set callback for allocation loss
 *   allocMgr.SetAllocationLostCallback([this](uint8_t ch, uint32_t bw) {
 *       StopStreaming();
 *       ShowError("FireWire bus reset - resources lost");
 *   });
 *
 *   // Allocate resources
 *   allocMgr.Allocate(5, 100, [](AllocationStatus status) {
 *       if (status == AllocationStatus::Success) {
 *           StartStreaming();
 *       }
 *   });
 *
 *   // Later, when bus reset occurs (called by topology layer):
 *   allocMgr.OnBusReset(newGeneration);
 *   // Manager automatically tries to reallocate channel 5 + 100 units
 *
 * Reference: Apple IOFireWireIRMAllocation class
 *            - Tracks allocation state (fIsochChannel, fBandwidthUnits, fAllocationGeneration)
 *            - handleBusReset() spawns thread to reallocate resources
 *            - failedToRealloc() calls fAllocationLostProc callback
 *
 *            Apple IOFireWireController::finishedBusScan()
 *            - Iterates fIRMAllocationsAllocated and calls handleBusReset()
 */
class IRMAllocationManager {
public:
    /**
     * Construct allocation manager.
     *
     * @param irmClient IRM client for performing allocations
     */
    explicit IRMAllocationManager(IRMClient& irmClient);
    ~IRMAllocationManager();

    /**
     * Set callback for allocation loss notification.
     *
     * Called when reallocation fails after bus reset.
     *
     * @param callback Callback function (nullptr = no callback)
     */
    void SetAllocationLostCallback(AllocationLostCallback callback);

    /**
     * Allocate channel and bandwidth resources.
     *
     * If allocation succeeds, resources are tracked and will be automatically
     * reallocated after bus reset.
     *
     * @param channel Channel number (0-63)
     * @param bandwidthUnits Bandwidth units to allocate
     * @param callback Completion callback
     * @param retryPolicy Retry configuration
     *
     * Note: Only one allocation is tracked at a time. Calling Allocate()
     *       while a previous allocation is active will release the previous
     *       allocation first.
     */
    void Allocate(uint8_t channel,
                 uint32_t bandwidthUnits,
                 AllocationCallback callback,
                 const RetryPolicy& retryPolicy = RetryPolicy::Default());

    /**
     * Release current allocation.
     *
     * Releases tracked resources and stops automatic reallocation.
     *
     * @param callback Completion callback
     * @param retryPolicy Retry configuration
     */
    void Release(AllocationCallback callback,
                const RetryPolicy& retryPolicy = RetryPolicy::Default());

    /**
     * Handle bus reset event.
     *
     * Called by topology layer after bus reset completes.
     * Automatically attempts to reallocate tracked resources.
     *
     * @param newGeneration New bus generation after reset
     *
     * Reference: Apple IOFireWireIRMAllocation::handleBusReset()
     */
    void OnBusReset(Generation newGeneration);

    /**
     * Check if resources are currently allocated.
     * @return true if allocation is active
     */
    [[nodiscard]] bool IsAllocated() const { return isAllocated_; }

    /**
     * Get allocated channel.
     * @return Channel number (0xFF = no channel)
     */
    [[nodiscard]] uint8_t GetChannel() const { return channel_; }

    /**
     * Get allocated bandwidth units.
     * @return Bandwidth units
     */
    [[nodiscard]] uint32_t GetBandwidthUnits() const { return bandwidthUnits_; }

    /**
     * Get allocation generation.
     * @return Generation when allocation succeeded
     */
    [[nodiscard]] Generation GetAllocationGeneration() const { return allocationGeneration_; }

private:
    IRMClient& irmClient_;

    // Allocation state
    bool isAllocated_{false};
    uint8_t channel_{0xFF};
    uint32_t bandwidthUnits_{0};
    Generation allocationGeneration_{0};

    // Callback for allocation loss
    AllocationLostCallback allocationLostCallback_;

    // Internal: Attempt to reallocate resources after bus reset
    void AttemptReallocation(Generation newGeneration);

    // Internal: Handle failed reallocation
    void OnReallocationFailed();
};

} // namespace ASFW::IRM
