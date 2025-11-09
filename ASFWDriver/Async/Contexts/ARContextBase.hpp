#pragma once

#include <cstdint>
#include <optional>

#include <DriverKit/IOLib.h>

#include "ContextBase.hpp"
#include "../Rings/BufferRing.hpp"
#include "../../Core/HardwareInterface.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Core/BarrierUtils.hpp"
#include "../../Core/OHCIConstants.hpp"

// Forward declare IOLock (included from IOLib.h)
struct IOLock;

namespace ASFW::Async {

// Use global OHCI bit constants as single source of truth
using ASFW::Driver::kContextControlWakeBit;

/**
 * \brief CRTP base class for OHCI AR (Asynchronous Receive) contexts.
 *
 * Provides common implementation for AR Request and AR Response contexts.
 * Uses BufferRing for managing INPUT_MORE descriptors in buffer-fill mode.
 *
 * \tparam Derived Concrete AR context class (ARRequestContext or ARResponseContext)
 * \tparam Tag Context role tag (ARRequestTag or ARResponseTag)
 *
 * \par OHCI Specification References
 * - §8.4.2: AR DMA operation (buffer-fill mode with INPUT_MORE descriptors)
 * - §8.6: AR interrupt handling (reqTxComplete, rspTxComplete)
 * - §C.3: Bus reset handling (AR contexts NOT stopped during reset)
 *
 * \par Design Rationale
 * AR contexts differ from AT contexts:
 * - **No descriptor chaining**: Use fixed buffers, not linked chains
 * - **No submission queue**: Hardware fills buffers automatically
 * - **Packet streams**: Each buffer may contain multiple packets
 * - **Bus reset resilience**: Keep running during reset per OHCI §C.3
 *
 * \par Apple Pattern
 * Based on AppleFWOHCI_AsyncReceive (AsyncReceive.cpp):
 * - allocatePacketBuffer(): Allocates fixed-size buffers
 * - getPacket(): Dequeues filled buffer and extracts packets
 * - updateResCount(): Recycles buffer descriptor
 * - Contexts run continuously, not stopped on bus reset
 *
 * \par Linux Pattern
 * See drivers/firewire/ohci.c:
 * - ar_context_init(): Initializes buffer ring with INPUT_MORE descriptors
 * - ar_context_run(): Arms context by writing CommandPtr and setting run bit
 * - handle_ar_packet(): Processes filled buffers in interrupt handler
 * - ar_context_add_page(): Recycles buffer by resetting descriptor
 *
 * \par Bus Reset Handling (CRITICAL)
 * Per OHCI §C.3: "Asynchronous receive contexts are not affected by bus reset.
 * The AR Request context MUST continue running to receive the synthetic bus-reset
 * packet and any PHY packets (if LinkControl.rcvPhyPkt=1)."
 *
 * This differs from AT contexts, which MUST be stopped during reset!
 */
template<typename Derived, ContextRole Tag>
class ARContextBase : public ContextBase<Derived, Tag> {
public:
    ARContextBase() = default;
    ~ARContextBase();

    /**
     * \brief Initialize AR context with hardware interface and buffer ring.
     *
     * \param hw Hardware register interface
     * \param bufferRing Initialized BufferRing for AR descriptors/buffers
     * \return kIOReturnSuccess on success, error code otherwise
     *
     * \par Implementation
     * 1. Call base Initialize() to set hw_ pointer
     * 2. Store bufferRing reference
     * 3. Allocate lock for context serialization
     *
     * \warning Caller must ensure bufferRing remains valid for context lifetime.
     */
    [[nodiscard]] kern_return_t Initialize(
        Driver::HardwareInterface& hw,
        BufferRing& bufferRing) noexcept;

    /**
     * \brief Arm AR context by writing CommandPtr and setting run bit.
     *
     * Starts hardware DMA operation by pointing CommandPtr to first descriptor
     * in buffer ring, then setting ContextControl.run bit.
     *
     * \param commandPtr Physical address of first descriptor with Z field encoded
     * \return kIOReturnSuccess on success, error code otherwise
     *
     * \par OHCI §8.2 / §8.4.2
     * CommandPtr format for AR contexts:
     * - Bits [31:4]: Physical address of first descriptor (16-byte aligned)
     * - Bit [0]: Z flag (1=continue, 0=last descriptor)
     *
     * For circular buffer rings, Z=1 indicates continuous operation.
     *
     * \par Implementation
     * 1. Verify context not already running
     * 2. Write CommandPtr register with descriptor address
     * 3. Set ContextControl.run bit
     * 4. Verify ContextControl.active becomes set
     *
     * \warning Must call Initialize() before Arm().
     */
    [[nodiscard]] kern_return_t Arm(uint32_t commandPtr) noexcept;

    /**
     * \brief Stop AR context with timeout.
     *
     * Clears ContextControl.run bit and waits for ContextControl.active to clear,
     * indicating hardware has finished processing current descriptor.
     *
     * \param timeoutMs Maximum time to wait for context to stop (milliseconds)
     * \return kIOReturnSuccess if stopped, kIOReturnTimeout if timeout
     *
     * \par OHCI §7.2.3 / §8.2
     * Stop sequence:
     * 1. Write 1 to ContextControl.Clear to clear run bit
     * 2. Poll ContextControl.active until it clears (hardware idle)
     * 3. Timeout if active doesn't clear within timeoutMs
     *
     * \par Bus Reset Warning (CRITICAL)
     * Per OHCI §C.3, AR contexts should NOT be stopped during bus reset!
     * - AR Request MUST keep running to receive synthetic bus-reset packet
     * - AR Response MAY be stopped, but usually kept running
     *
     * This method is provided for shutdown/error recovery, NOT for bus reset handling.
     */
    [[nodiscard]] kern_return_t Stop(uint32_t timeoutMs = 100) noexcept;

    /**
     * \brief Dequeue next filled buffer from ring.
     *
     * Checks if hardware has filled any buffers and returns information
     * about the next available buffer.
     *
     * \return FilledBufferInfo if buffer available, nullopt if none ready
     *
     * \par Implementation
     * 1. Acquire lock to serialize dequeue operations
     * 2. Call bufferRing_.Dequeue() to check for filled buffer
     * 3. Return buffer info (caller must parse packets and call Recycle)
     *
     * \par Thread Safety
     * Uses lock_ to serialize dequeue/recycle operations. Safe to call
     * from interrupt context.
     */
    [[nodiscard]] std::optional<FilledBufferInfo> Dequeue() noexcept;

    /**
     * \brief Recycle buffer descriptor for reuse by hardware.
     *
     * Resets descriptor statusWord to empty state and signals hardware
     * that buffer is available for writing.
     *
     * \param index Descriptor index from FilledBufferInfo
     * \return kIOReturnSuccess on success, error code otherwise
     *
     * \par Implementation
     * 1. Acquire lock to serialize with dequeue
     * 2. Call bufferRing_.Recycle(index) to reset descriptor
     * 3. Write ContextControl.wake bit to signal hardware
     * 4. Release fence ensures descriptor update visible before wake
     *
     * \par OHCI §8.4.2
     * "After software processes a buffer, it recycles the descriptor by
     * resetting resCount to reqCount. Software then writes the wake bit
     * to notify hardware that descriptors are available."
     */
    [[nodiscard]] kern_return_t Recycle(size_t index) noexcept;

    /**
     * \brief Get reference to underlying buffer ring.
     *
     * \return BufferRing reference
     */
    [[nodiscard]] BufferRing& GetBufferRing() noexcept {
        return *bufferRing_;
    }

    [[nodiscard]] const BufferRing& GetBufferRing() const noexcept {
        return *bufferRing_;
    }

protected:
    using ContextBase<Derived, Tag>::hw_;

private:
    BufferRing* bufferRing_{nullptr};  ///< AR buffer ring (externally owned)
    IOLock* lock_{nullptr};             ///< Serializes dequeue/recycle operations
};

// ============================================================================
// Template Method Implementations
// ============================================================================

template<typename Derived, ContextRole Tag>
ARContextBase<Derived, Tag>::~ARContextBase() {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

template<typename Derived, ContextRole Tag>
kern_return_t ARContextBase<Derived, Tag>::Initialize(
    Driver::HardwareInterface& hw,
    BufferRing& bufferRing) noexcept {

    // Initialize base class
    kern_return_t result = ContextBase<Derived, Tag>::Initialize(hw);
    if (result != kIOReturnSuccess) {
        return result;
    }

    // Allocate lock for dequeue/recycle serialization
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG(Async, "%{public}s: failed to allocate lock",
                 this->ContextName().data());
        return kIOReturnNoMemory;
    }

    bufferRing_ = &bufferRing;

    ASFW_LOG(Async, "%{public}s: initialized with %zu buffers x %zu bytes",
             this->ContextName().data(),
             bufferRing.BufferCount(),
             bufferRing.BufferSize());

    return kIOReturnSuccess;
}

template<typename Derived, ContextRole Tag>
kern_return_t ARContextBase<Derived, Tag>::Arm(uint32_t commandPtr) noexcept {
    if (!this->hw_) {
        ASFW_LOG(Async, "%{public}s: Arm called before Initialize",
                 this->ContextName().data());
        return kIOReturnNotReady;
    }

    // Check if already running
    if (this->IsRunning()) {
        ASFW_LOG(Async, "%{public}s: already running", this->ContextName().data());
        return kIOReturnBusy;
    }

    // Publish all descriptors before arming (flush after Finalize)
    if (bufferRing_) {
        bufferRing_->PublishAllDescriptorsOnce();
    }

    // Write CommandPtr register with descriptor address
    // Per OHCI §8.2: CommandPtr[31:4] = physAddr[31:4], CommandPtr[0] = Z
    this->WriteCommandPtr(commandPtr);

    // Set ContextControl.run bit to start DMA
    // Per OHCI §8.2: run bit = bit 15 of ContextControl
    constexpr uint32_t kRunBit = 1u << 15;
    this->WriteControlSet(kRunBit);

    // Verify context became active
    // Poll briefly to confirm hardware started processing
    // Increased timeout to 50ms - hardware may not activate until after LinkEnable + bus reset
    for (int i = 0; i < 50; ++i) {
        if (this->IsActive()) {
            ASFW_LOG(Async, "%{public}s: armed and active (CommandPtr=0x%08x)",
                     this->ContextName().data(), commandPtr);
            return kIOReturnSuccess;
        }
        IOSleep(1);  // 1ms delay
    }

    ASFW_LOG(Async, "%{public}s: armed (info: not active yet after 50ms, may activate after reset)",
             this->ContextName().data());
    // Not fatal - hardware may start later
    return kIOReturnSuccess;
}

template<typename Derived, ContextRole Tag>
kern_return_t ARContextBase<Derived, Tag>::Stop(uint32_t timeoutMs) noexcept {
    if (!this->hw_) {
        return kIOReturnNotReady;
    }

    // Check if already stopped
    if (!this->IsRunning()) {
        return kIOReturnSuccess;
    }

    // Clear ContextControl.run bit
    // Per OHCI §7.2.3 / §8.2: write 1 to ContextControl.Clear
    constexpr uint32_t kRunBit = 1u << 15;
    this->WriteControlClear(kRunBit);

    // Poll ContextControl.active until it clears
    // Per OHCI §7.2.3: active=1 while hardware processes descriptors
    constexpr uint32_t kPollIntervalMs = 1;

    for (uint32_t elapsed = 0; elapsed < timeoutMs; elapsed += kPollIntervalMs) {
        if (!this->IsActive()) {
            ASFW_LOG(Async, "%{public}s: stopped after %u ms",
                     this->ContextName().data(), elapsed);
            return kIOReturnSuccess;
        }
        IOSleep(kPollIntervalMs);
    }

    ASFW_LOG(Async, "%{public}s: stop timeout after %u ms (still active)",
             this->ContextName().data(), timeoutMs);
    return kIOReturnTimeout;
}

template<typename Derived, ContextRole Tag>
std::optional<FilledBufferInfo> ARContextBase<Derived, Tag>::Dequeue() noexcept {
    if (!bufferRing_ || !lock_) {
        return std::nullopt;
    }

    // Acquire lock to serialize dequeue/recycle operations
    IOLockLock(lock_);

    // CRITICAL INVESTIGATION: ReadBarrier() is DISABLED for uncached DMA memory
    //
    // PROBLEM: DMA descriptors are mapped as DEVICE MEMORY (kIOMemoryMapCacheModeInhibit)
    // - ReadBarrier() → memory_order_acquire → ARM64 DMB ISHLD (Data Memory Barrier)
    // - DMB is for NORMAL MEMORY (cached memory with coherency protocol)
    // - DEVICE MEMORY requires DSB (Device Synchronization Barrier), NOT DMB
    //
    // ISSUE: On ARM64 weak ordering, DMB does NOT synchronize with device memory accesses!
    // - bufferRing_->Dequeue() → FetchRange() → IoBarrier() → DSB (correct for device memory)
    // - ReadBarrier() → DMB (for normal memory, may NOT wait for DSB to complete!)
    // - Result: CPU may reorder descriptor load BEFORE DSB completes
    //
    // CONSEQUENCE: Adding DMB before FetchRange may allow CPU to speculatively load
    // descriptor from cache BEFORE FetchRange's DSB completes, reading STALE data!
    //
    // FetchRange() alone provides sufficient ordering via DSB. Adding ReadBarrier may
    // actually CAUSE cache coherency issues, not fix them!
    //
    // See: ANALYSIS_DMA_BARRIERS_AND_CACHE_COHERENCY.md for full technical explanation
    //
    // Driver::ReadBarrier();  // ⚠️  DISABLED: May cause reordering with device memory on ARM64

    // Check for filled buffer
    // NOTE: bufferRing_->Dequeue() calls FetchRange() internally, which provides
    // the correct DSB barrier for device memory access
    std::optional<FilledBufferInfo> result = bufferRing_->Dequeue();

    IOLockUnlock(lock_);

    return result;
}

template<typename Derived, ContextRole Tag>
kern_return_t ARContextBase<Derived, Tag>::Recycle(size_t index) noexcept {
    if (!bufferRing_ || !lock_ || !this->hw_) {
        return kIOReturnNotReady;
    }

    // Acquire lock to serialize with dequeue
    IOLockLock(lock_);

    // Recycle buffer descriptor
    kern_return_t result = bufferRing_->Recycle(index);

    if (result == kIOReturnSuccess) {
        // Release fence: ensure descriptor update is visible to hardware
        Driver::WriteBarrier();

        // Write ContextControl.wake bit to signal hardware
        // Use global constant (bit 12 = 0x1000, verified against Linux/Apple/OHCI spec)
        this->WriteControlSet(kContextControlWakeBit);

        // DIAGNOSTIC: Log wake bit write to trace hardware notification
        ASFW_LOG(Async,
                 "♻️  %{public}s: Wrote WAKE bit after recycling buffer[%zu]",
                 this->ContextName().data(), index);
    } else {
        ASFW_LOG(Async,
                 "⚠️  %{public}s: Recycle failed for buffer[%zu], kr=0x%08x (wake NOT written)",
                 this->ContextName().data(), index, result);
    }

    IOLockUnlock(lock_);

    return result;
}

} // namespace ASFW::Async
