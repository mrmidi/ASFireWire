#pragma once

#include "IRMTypes.hpp"
#include "../Async/Interfaces/IFireWireBusOps.hpp"
#include <functional>
#include <memory>
#include <array>
// #include <libkern/OSByteOrder.h>

namespace ASFW::IRM {

/**
 * Callback for IRM allocation operations.
 * Invoked asynchronously when allocation completes (success or failure).
 *
 * @param status Result of allocation operation
 */
using AllocationCallback = std::function<void(AllocationStatus status)>;

/**
 * IRMClient - Allocates isochronous resources from IRM node.
 *
 * This is the **client-only** implementation (Phase 1 from IRM-Implementation-Plan.md).
 * It allocates channels and bandwidth from an external IRM node on the bus.
 *
 * Design Philosophy (from IRM_FINAL_THOUGHTS.md §3.2):
 * - IRMClient does ONLY allocation policy (compute bit masks, CAS, retry logic)
 * - Does NOT own generation or IRM node ID (passed from topology layer)
 * - Does NOT create IRM-specific generation types (uses shared Generation)
 * - Properly maps outcomes to AllocationStatus (no hidden meanings)
 *
 * Responsibilities:
 * 1. Compute bit masks for channels (1 = free, 0 = allocated)
 * 2. Read CHANNELS_AVAILABLE / BANDWIDTH_AVAILABLE registers
 * 3. Perform compare-and-swap (CAS) lock transactions
 * 4. Handle contention with retry loop (2-3 tries)
 * 5. Map outcomes to AllocationStatus
 *
 * Does NOT:
 * - Own or guess IRM node ID (set by topology layer)
 * - Own or guess bus generation (passed to each operation)
 * - Maintain any IRM-internal FSM beyond retry loop
 * - Create its own concept of "allocation generation"
 *
 * Usage Example:
 *   // After bus reset and topology scan:
 *   irmClient.SetIRMNode(irmNodeId, currentGeneration);
 *
 *   // When audio engine wants to start streaming:
 *   uint32_t bandwidth = CalculateBandwidthUnits(bitsPerSec, 400);
 *   irmClient.AllocateResources(5, bandwidth,
 *       [this](AllocationStatus status) {
 *           if (status == AllocationStatus::Success) {
 *               StartIsochTransmission();
 *           }
 *       });
 *
 * Reference: Apple IOFireWireIRMAllocation class
 *            Apple IOFireWireController allocateIRMChannelInGeneration()
 *            Linux firewire-core-cdev.c iso resource management
 */
class IRMClient {
public:
    /**
     * Construct IRM client with bus operations interface.
     *
     * @param busOps Canonical async bus operations (Async::IFireWireBusOps)
     */
    explicit IRMClient(Async::IFireWireBusOps& busOps);
    ~IRMClient();

    /**
     * Initialize with current IRM node and generation.
     *
     * Called by topology layer after bus reset and Self-ID processing.
     * IRM node = highest node ID with Contender bit (C bit) set in Self-ID.
     *
     * @param irmNodeId IRM node ID (0xFF = no IRM on bus)
     * @param generation Current bus generation
     *
     * Reference: Apple IOFireWireController::processSelfIDs()
     *            Linux fw_core_handle_bus_reset() - finds IRM node
     */
    void SetIRMNode(uint8_t irmNodeId, Generation generation);

    /**
     * Allocate specific isochronous channel (0-63).
     *
     * Operation:
     * 1. Read CHANNELS_AVAILABLE register
     * 2. Check if channel bit is set (available)
     * 3. Clear bit via CAS lock
     * 4. Retry on contention (up to RetryPolicy::maxRetries)
     *
     * @param channel Channel number (0-63)
     * @param callback Completion callback with allocation status
     * @param retryPolicy Retry configuration (default: 2 retries)
     *
     * Callback status codes:
     * - Success: Channel allocated successfully
     * - NoResources: Channel already allocated by another node
     * - GenerationMismatch: Bus reset occurred, operation aborted
     * - Timeout: IRM node didn't respond
     * - NotFound: No IRM node on bus
     * - Failed: Unexpected error
     *
     * Reference: Apple IOFireWireController::allocateIRMChannelInGeneration()
     */
    void AllocateChannel(uint8_t channel,
                        AllocationCallback callback,
                        const RetryPolicy& retryPolicy = RetryPolicy::Default());

    /**
     * Release previously allocated channel.
     *
     * Operation:
     * 1. Read CHANNELS_AVAILABLE register
     * 2. Set bit via CAS lock (mark available)
     * 3. Retry on contention
     *
     * @param channel Channel number (0-63)
     * @param callback Completion callback
     * @param retryPolicy Retry configuration
     *
     * Reference: Apple IOFireWireController::releaseIRMChannelInGeneration()
     */
    void ReleaseChannel(uint8_t channel,
                       AllocationCallback callback,
                       const RetryPolicy& retryPolicy = RetryPolicy::Default());

    /**
     * Allocate bandwidth units.
     *
     * Operation:
     * 1. Read BANDWIDTH_AVAILABLE register
     * 2. Check if sufficient units available (current >= requested)
     * 3. Subtract units via CAS lock
     * 4. Retry on contention
     *
     * @param units Bandwidth units to allocate
     * @param callback Completion callback
     * @param retryPolicy Retry configuration
     *
     * Callback status codes:
     * - Success: Bandwidth allocated successfully
     * - NoResources: Insufficient bandwidth available
     * - GenerationMismatch: Bus reset occurred
     * - Timeout/NotFound/Failed: As above
     *
     * Reference: Apple IOFireWireController::allocateIRMBandwidthInGeneration()
     */
    void AllocateBandwidth(uint32_t units,
                          AllocationCallback callback,
                          const RetryPolicy& retryPolicy = RetryPolicy::Default());

    /**
     * Release bandwidth units.
     *
     * Operation:
     * 1. Read BANDWIDTH_AVAILABLE register
     * 2. Add units via CAS lock
     * 3. Retry on contention
     *
     * @param units Bandwidth units to release
     * @param callback Completion callback
     * @param retryPolicy Retry configuration
     *
     * Reference: Apple IOFireWireController::releaseIRMBandwidthInGeneration()
     */
    void ReleaseBandwidth(uint32_t units,
                         AllocationCallback callback,
                         const RetryPolicy& retryPolicy = RetryPolicy::Default());

    /**
     * Allocate both channel and bandwidth atomically (two-phase commit).
     *
     * Operation:
     * 1. Allocate channel
     * 2. If channel succeeds, allocate bandwidth
     * 3. If bandwidth fails, release channel (rollback)
     * 4. Return combined result
     *
     * This ensures atomicity: either both succeed, or neither is allocated.
     *
     * @param channel Channel number (0-63)
     * @param bandwidthUnits Bandwidth units to allocate
     * @param callback Completion callback
     * @param retryPolicy Retry configuration (applies to both operations)
     *
     * Callback status reflects final state:
     * - Success: Both channel and bandwidth allocated
     * - NoResources: Either channel unavailable or insufficient bandwidth
     *                (channel is released if bandwidth fails)
     * - Other codes: As above
     *
     * Reference: Apple IOFireWireIRMAllocation::allocateIsochResources()
     *            Implements two-phase commit with automatic rollback
     */
    void AllocateResources(uint8_t channel,
                          uint32_t bandwidthUnits,
                          AllocationCallback callback,
                          const RetryPolicy& retryPolicy = RetryPolicy::Default());

    /**
     * Release both channel and bandwidth.
     *
     * Both operations are performed independently (not atomic).
     * Callback is invoked after both complete.
     *
     * @param channel Channel number (0-63)
     * @param bandwidthUnits Bandwidth units to release
     * @param callback Completion callback
     * @param retryPolicy Retry configuration
     *
     * Reference: Apple IOFireWireIRMAllocation::deallocateIsochResources()
     */
    void ReleaseResources(uint8_t channel,
                         uint32_t bandwidthUnits,
                         AllocationCallback callback,
                         const RetryPolicy& retryPolicy = RetryPolicy::Default());

    /**
     * Get current IRM node ID.
     * @return IRM node ID (0xFF = no IRM on bus)
     */
    [[nodiscard]] uint8_t GetIRMNodeID() const { return irmNodeId_; }

    /**
     * Get current generation.
     * @return Bus generation
     */
    [[nodiscard]] Generation GetGeneration() const { return generation_; }

private:
    Async::IFireWireBusOps& busOps_;   ///< Canonical bus operations interface

    uint8_t irmNodeId_{0xFF};          ///< Current IRM node ID (0xFF = no IRM)
    Generation generation_{0};         ///< Current bus generation

    // -------------------------------------------------------------------------
    // Internal Helper Methods
    // -------------------------------------------------------------------------

    /**
     * Read quadlet from IRM CSR space (helper wrapper).
     *
     * @param addressLo CSR address low 32 bits
     * @param callback Completion callback (success, value)
     */
    void ReadIRMQuadlet(
        uint32_t addressLo,
        std::function<void(bool success, uint32_t value)> callback);

    /**
     * Perform compare-and-swap on IRM CSR space (helper wrapper).
     *
     * @param addressLo CSR address low 32 bits
     * @param expected Expected old value (host byte order)
     * @param desired Desired new value (host byte order)
     * @param callback Completion callback (success, old value)
     */
    void CompareSwapIRMQuadlet(
        uint32_t addressLo,
        uint32_t expected,
        uint32_t desired,
        std::function<void(bool success, uint32_t oldValue)> callback);

    // Internal implementation methods (see IRMClient.cpp)
    void PerformChannelLock(uint8_t channel, bool allocate,
                           AllocationCallback callback,
                           const RetryPolicy& retryPolicy);

    void PerformBandwidthLock(uint32_t units, bool allocate,
                             AllocationCallback callback,
                             const RetryPolicy& retryPolicy);
};

} // namespace ASFW::IRM
