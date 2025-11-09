#pragma once

#include <cstdint>
#include <string_view>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>
#include "../../Logging/Logging.hpp"
#include "../../Core/BarrierUtils.hpp"

namespace ASFW::Async::Engine {

/**
 * DmaContextManagerBase - Templated base for shared DMA context lifecycle
 * 
 * Provides common operations shared by AT and AR managers:
 * - State transitions with logging
 * - Lock management
 * - I/O barriers (read/write fences)
 * - Active bit polling
 * - Initialization delegation
 * 
 * @tparam ContextT Context type (ATRequestContext, ARRequestContext, etc.)
 * @tparam RingT Ring type (DescriptorRing, BufferRing)
 * @tparam RoleTag Role tag for context name (ATRequestTag, ARRequestTag, etc.)
 * @tparam PolicyT Policy type providing State enum and initial state
 */
template<typename ContextT, typename RingT, typename RoleTag, typename PolicyT>
class DmaContextManagerBase {
public:
    using StateEnum = typename PolicyT::State;

    /**
     * Constructor: Initialize base with context and ring references
     */
    DmaContextManagerBase(ContextT& ctx, RingT& ring)
        : ctx_(ctx), ring_(ring), state_(PolicyT::kInitialState)
    {
        lock_ = IOLockAlloc();
        if (!lock_) {
            // Log but don't fail - lock may be allocated later
            ASFW_LOG_ERROR(Async, "[%{public}s] Failed to allocate lock", RoleTag::kContextName.data());
        }
    }

    /**
     * Destructor: Free lock if allocated
     */
    ~DmaContextManagerBase() {
        if (lock_) {
            IOLockFree(lock_);
            lock_ = nullptr;
        }
    }

    /**
     * Initialize: Delegate to context's Initialize method
     */
    [[nodiscard]] kern_return_t Initialize(Driver::HardwareInterface& hw) {
        return ctx_.Initialize(hw);
    }

    /**
     * Get current state (thread-safe read)
     */
    [[nodiscard]] StateEnum GetState() const noexcept {
        IOLockLock(lock_);
        StateEnum s = state_;
        IOLockUnlock(lock_);
        return s;
    }

protected:
    /**
     * Transition to new state with logging
     * Thread-safe: must hold lock_ before calling
     */
    void Transition(StateEnum newState, uint32_t txid, const char* why) {
        state_ = newState;
        ASFW_LOG_KV(Async, RoleTag::kContextName.data(), txid, 0, "state=%{public}s: %{public}s (head=%lu tail=%lu)", PolicyT::ToStr(newState), why, (unsigned long)ring_.Head(), (unsigned long)ring_.Tail());
    }

    /**
     * Poll for ACTIVE bit to become set (for WAKE confirmation)
     * @param usMax Maximum microseconds to poll
     * @return true if ACTIVE became set, false on timeout
     */
    bool PollActiveUs(uint32_t usMax) {
        for (uint32_t i = 0; i < usMax; ++i) {
            if (ctx_.IsActive()) {
                return true;
            }
            IODelay(1);
        }
        return false;
    }

    /**
     * I/O write fence: Ensures all writes complete before proceeding
     * Use before setting RUN/WAKE bits after descriptor publishing
     */
    void IoWriteFence() {
        Driver::WriteBarrier();
    }

    /**
     * I/O read fence: Ensures all reads see latest writes
     * Use after clearing RUN bit to ensure hardware sees the change
     */
    void IoReadFence() {
        Driver::ReadBarrier();
    }

    /**
     * Get current timestamp in microseconds (monotonic)
     */
    static uint64_t NowUs() {
        static mach_timebase_info_data_t tb{0, 0};
        if (!tb.denom) {
            mach_timebase_info(&tb);
        }
        const uint64_t t = mach_absolute_time();
        return (t * tb.numer) / tb.denom;
    }

    // Protected members for derived classes
    ContextT& ctx_;
    RingT& ring_;
    StateEnum state_;
    IOLock* lock_;

private:
    // Disable copy/move
    DmaContextManagerBase(const DmaContextManagerBase&) = delete;
    DmaContextManagerBase& operator=(const DmaContextManagerBase&) = delete;
    DmaContextManagerBase(DmaContextManagerBase&&) = delete;
    DmaContextManagerBase& operator=(DmaContextManagerBase&&) = delete;
};

} // namespace ASFW::Async::Engine
