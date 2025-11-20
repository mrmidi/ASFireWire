#include "IRMAllocationManager.hpp"
#include "../Logging/Logging.hpp"
#include <os/log.h>

namespace ASFW::IRM {

// ============================================================================
// Constructor / Destructor
// ============================================================================

IRMAllocationManager::IRMAllocationManager(IRMClient& irmClient)
    : irmClient_(irmClient)
{
}

IRMAllocationManager::~IRMAllocationManager() = default;

// ============================================================================
// Configuration
// ============================================================================

void IRMAllocationManager::SetAllocationLostCallback(AllocationLostCallback callback) {
    allocationLostCallback_ = callback;
}

// ============================================================================
// Allocation (Modern C++23 with reduced duplication)
// ============================================================================

void IRMAllocationManager::Allocate(uint8_t channel,
                                     uint32_t bandwidthUnits,
                                     AllocationCallback callback,
                                     const RetryPolicy& retryPolicy)
{
    // Helper lambda to update allocation state on success (eliminates duplication)
    auto updateAllocationState = [this, channel, bandwidthUnits, callback](AllocationStatus status) {
        if (status == AllocationStatus::Success) {
            // Track allocation for automatic reallocation
            isAllocated_ = true;
            channel_ = channel;
            bandwidthUnits_ = bandwidthUnits;
            allocationGeneration_ = irmClient_.GetGeneration();

            ASFW_LOG(IRM, "AllocationManager: Allocated channel %u, %u bandwidth units, gen %u",
                     channel_, bandwidthUnits_, allocationGeneration_);
        }
        callback(status);
    };

    // If already allocated, release first
    if (isAllocated_) {
        ASFW_LOG(IRM, "AllocationManager: Releasing previous allocation before new allocation");

        Release([this, channel, bandwidthUnits, updateAllocationState, retryPolicy](AllocationStatus) {
            // Ignore release status, proceed with new allocation
            irmClient_.AllocateResources(channel, bandwidthUnits, updateAllocationState, retryPolicy);
        }, retryPolicy);
    } else {
        // No previous allocation, allocate directly
        irmClient_.AllocateResources(channel, bandwidthUnits, updateAllocationState, retryPolicy);
    }
}

void IRMAllocationManager::Release(AllocationCallback callback,
                                    const RetryPolicy& retryPolicy)
{
    if (!isAllocated_) {
        ASFW_LOG(IRM, "AllocationManager: No allocation to release");
        callback(AllocationStatus::Success);
        return;
    }

    const uint8_t channelToRelease = channel_;
    const uint32_t bandwidthToRelease = bandwidthUnits_;

    ASFW_LOG(IRM, "AllocationManager: Releasing channel %u, %u bandwidth units",
             channelToRelease, bandwidthToRelease);

    // Clear allocation state immediately (don't reallocate after release)
    isAllocated_ = false;
    channel_ = 0xFF;
    bandwidthUnits_ = 0;
    allocationGeneration_ = 0;

    // Release resources
    irmClient_.ReleaseResources(channelToRelease, bandwidthToRelease,
        [callback](AllocationStatus status) {
            callback(status);
        },
        retryPolicy);
}

// ============================================================================
// Bus Reset Handling
// ============================================================================

void IRMAllocationManager::OnBusReset(Generation newGeneration) {
    if (!isAllocated_) {
        // No active allocation, nothing to do
        return;
    }

    if (allocationGeneration_ == newGeneration) {
        // Generation hasn't changed (shouldn't happen, but check anyway)
        ASFW_LOG(IRM, "AllocationManager: OnBusReset called with same generation %u",
                 newGeneration);
        return;
    }

    ASFW_LOG(IRM, "AllocationManager: Bus reset detected (gen %u -> %u), attempting reallocation",
             allocationGeneration_, newGeneration);

    // Attempt to reallocate same resources
    AttemptReallocation(newGeneration);
}

// ============================================================================
// Internal Implementation
// ============================================================================

void IRMAllocationManager::AttemptReallocation(Generation newGeneration) {
    if (!isAllocated_) {
        return;
    }

    const uint8_t channelToRealloc = channel_;
    const uint32_t bandwidthToRealloc = bandwidthUnits_;

    ASFW_LOG(IRM, "AllocationManager: Attempting to reallocate channel %u, %u bandwidth units",
             channelToRealloc, bandwidthToRealloc);

    // Try to allocate same resources in new generation
    // Note: Don't use the manager's Allocate() method - that would recursively
    // update our state. Instead, call IRMClient directly and only update state
    // on success.
    irmClient_.AllocateResources(channelToRealloc, bandwidthToRealloc,
        [this, newGeneration, channelToRealloc, bandwidthToRealloc](AllocationStatus status) {
            if (status == AllocationStatus::Success) {
                // Reallocation succeeded! Update generation
                allocationGeneration_ = newGeneration;

                ASFW_LOG(IRM, "AllocationManager: Reallocation succeeded "
                         "(channel %u, %u bandwidth units, gen %u)",
                         channelToRealloc, bandwidthToRealloc, newGeneration);
            } else if (status == AllocationStatus::GenerationMismatch) {
                // Another bus reset occurred during reallocation
                // This is handled by another OnBusReset() call, so just log
                ASFW_LOG(IRM, "AllocationManager: Reallocation aborted due to another bus reset");
            } else {
                // Reallocation failed permanently
                ASFW_LOG_ERROR(IRM, "AllocationManager: Reallocation failed with status %u",
                               (uint32_t)status);

                OnReallocationFailed();
            }
        },
        RetryPolicy::Default());
}

void IRMAllocationManager::OnReallocationFailed() {
    const uint8_t lostChannel = channel_;
    const uint32_t lostBandwidth = bandwidthUnits_;

    // Clear allocation state
    isAllocated_ = false;
    channel_ = 0xFF;
    bandwidthUnits_ = 0;
    allocationGeneration_ = 0;

    ASFW_LOG_ERROR(IRM, "AllocationManager: Allocation lost (channel %u, %u bandwidth units)",
                   lostChannel, lostBandwidth);

    // Notify client
    if (allocationLostCallback_) {
        allocationLostCallback_(lostChannel, lostBandwidth);
    }
}

} // namespace ASFW::IRM
