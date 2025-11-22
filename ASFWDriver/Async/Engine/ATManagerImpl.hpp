#pragma once

#include "ATManager.hpp"
#include "ATTrace.hpp"
#include "../Contexts/ContextBase.hpp"  // For ATRequestTag, ATResponseTag full definitions
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../Core/LockPolicy.hpp"  // Phase 1.2: Fine-grained locking
#include "../../Logging/LogConfig.hpp"  // For ASFW_LOG_V1 macro

using ASFW::Driver::kContextControlRunBit;
using ASFW::Driver::kContextControlWakeBit;
using ASFW::Driver::kContextControlDeadBit;
using ASFW::Driver::kContextControlActiveBit;

namespace ASFW::Async::Engine {

namespace {
inline const char* ToStringImpl(ATState s) noexcept {
    switch (s) {
        case ATState::IDLE: return "IDLE";
        case ATState::ARMING: return "ARMING";
        case ATState::RUNNING: return "RUNNING";
        case ATState::STOPPING: return "STOPPING";
        case ATState::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
} // anonymous namespace

inline const char* ToString(ATState s) noexcept {
    return ToStringImpl(s);
}

template<typename ContextT, typename RingT, typename RoleTag>
kern_return_t ATManager<ContextT, RingT, RoleTag>::Submit(DescriptorChain&& chain, const AsyncCmdOptions& opts) {
    if (chain.Empty()) {
        ASFW_LOG_ERROR(Async, "[%{public}s] Submit: Empty chain", RoleTag::kContextName.data());
        return kIOReturnBadArgument;
    }

    const uint32_t txid = chain.txid;

    // PATH decision using software state only (Apple's pattern)
    // From decompilation @ 0xDBBE line 109: if (*((_BYTE *)this + 28))
    // Apple checks ONLY software flag, never reads hardware registers
    bool canP2;
    {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);

        // Simple check: Is context marked as running in software?
        // Additional safety: Ring must still have descriptors we can link to
        const bool hasPrevLast = ring().PrevLastBlocks() > 0;
        const bool ringHasData = !ring().IsEmpty();
        canP2 = (this->state_ == State::RUNNING) && hasPrevLast && ringHasData;
    }  // Lock automatically released here (RAII)

    if (canP2) {
        // PATH 2: Hot-append to running context (fire-and-forget)
        kern_return_t kr = SubmitPath2_(chain, txid, opts);
        if (kr == kIOReturnSuccess) {
            // FIX: Removed duplicate requestStop_ call - was being called twice!
            // Also removed to prevent deadlock (same reason as PATH 1/2 inline fixes)
            // if (opts.needsFlush) {
            //     // Re-acquire lock only for requestStop_
            //     IOLockWrapper lockWrapper(lock());
            //     ScopedLock guard(lockWrapper);
            //     requestStop_(txid, "needsFlush");
            // }
            return kIOReturnSuccess;
        }
        // Fall through to PATH 1 fallback on failure
        ASFW_LOG(Async, "[%{public}s] PATH 2 failed, falling back to PATH 1", RoleTag::kContextName.data());
    }

    // V1: Compact AT transmit one-liner for packet flow visibility
    const uint8_t totalBlocks = chain.TotalBlocks();
    ASFW_LOG_V1(Async, "📤 AT/TX: txid=%u blocks=%u (%{public}s)",
               txid, totalBlocks, canP2 ? "PATH2" : "PATH1");

    // PATH 1: First submission or re-arm
    // Lock held only during FSM state updates, NOT during hardware operations
    return SubmitPath1_(chain, txid, opts);
}

template<typename ContextT, typename RingT, typename RoleTag>
kern_return_t ATManager<ContextT, RingT, RoleTag>::SubmitPath1_(const DescriptorChain& chain, uint32_t txid, const AsyncCmdOptions& opts) {
    // Phase 1.2: Fine-grained locking for PATH 1
    // Lock held only for FSM transitions and ring updates, NOT for hardware operations

    // FSM transition under lock
    {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);
        Base::Transition(State::ARMING, txid, "path1_start");
    }

    // Hardware operations WITHOUT lock
    // Z nibble must be total blocks per OHCI; use TotalBlocks() not firstBlocks
    const uint8_t z = ATSubmitPolicy::ComputeZ(chain.TotalBlocks());
    PublishChain_(chain);
    this->IoWriteFence();

    // If hardware still considers the context running (PATH-2 fallback case),
    // clear RUN before programming CommandPtr so the next RUN=1 transition is visible.
    if (ctx().IsRunning()) {
        clearRunAndPoll_();
    }

    const uint32_t cmdPtr = ring().CommandPtrWordFromIOVA(chain.firstIOVA32, z);
    if (cmdPtr == 0) {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);
        Base::Transition(State::ERROR, txid, "invalid_cmdptr");
        return kIOReturnBadArgument;
    }

    // Program hardware WITHOUT holding lock
    ctx().WriteCommandPtr(cmdPtr);
    ctx().WriteControlSet(kContextControlRunBit);

    ASFW_LOG_KV(Async, RoleTag::kContextName.data(), txid, generation_, "P1_ARM head=%lu tail=%lu z=%u cmdPtr=0x%08x", (unsigned long)ring().Head(), (unsigned long)ring().Tail(), z, cmdPtr);

    trace_.push({NowNs(), txid, generation_, ATEvent::P1_ARM, cmdPtr, z});

    // FSM transition and ring update under lock
    {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);
        Base::Transition(State::RUNNING, txid, "path1_armed");
        UpdateRingTail_(chain);

        // FIX: Don't call requestStop_ with lock held - causes deadlock!
        // Same fix as PATH 2 - let interrupt handler stop context when appropriate
        // if (opts.needsFlush) {
        //     requestStop_(txid, "needsFlush");
        // }
    }

    return kIOReturnSuccess;
}

template<typename ContextT, typename RingT, typename RoleTag>
kern_return_t ATManager<ContextT, RingT, RoleTag>::SubmitPath2_(const DescriptorChain& chain, uint32_t txid, const AsyncCmdOptions& opts) {
    // PATH 2: Hot-append to running context (Apple's fire-and-forget pattern)
    // Lock held only for ring updates, NOT during hardware operations
    // WAKE is pulsed without polling, allowing immediate return

    // Ring updates under lock
    kern_return_t linkResult = kIOReturnSuccess;
    {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);
        linkResult = LinkTailTo_(chain);
        if (linkResult == kIOReturnSuccess) {
            PublishPrevLast_(chain);
        }
    }  // Lock released before hardware operations

    if (linkResult != kIOReturnSuccess) {
        ASFW_LOG_KV(Async,
                    RoleTag::kContextName.data(),
                    txid,
                    generation_,
                    "P2_FALLBACK cause=%{public}s",
                    "LinkTailTo");
        trace_.push({NowNs(), txid, generation_, ATEvent::P2_FALLBACK, 0, 0});
        return linkResult;
    }

    // Hardware operations WITHOUT holding lock
    this->IoWriteFence();

    // WAKE guard: Check RUN==1 && DEAD==0 before pulsing WAKE
    const uint32_t ctrl = ctx().ReadControl();
    const bool run = (ctrl & kContextControlRunBit) != 0;
    const bool dead = (ctrl & kContextControlDeadBit) != 0;

    ASFW_LOG_KV(Async, RoleTag::kContextName.data(), txid, generation_, "WAKE_GUARD ctrl=0x%08x run=%d dead=%d", ctrl, run ? 1 : 0, dead ? 1 : 0);

    if (!run || dead) {
        ASFW_LOG_KV(Async, RoleTag::kContextName.data(), txid, generation_, "P2_FALLBACK cause=%{public}s", !run ? "RUN0" : "DEAD");
        UnlinkTail_();
        trace_.push({NowNs(), txid, generation_, ATEvent::P2_FALLBACK, ctrl, 0});
        return kIOReturnNotReady;
    }

    // Pulse WAKE bit and return immediately (Apple's fire-and-forget pattern)
    // From decompilation @ 0xDBBE: WAKE is a hint, hardware picks up branch asynchronously
    // NO POLLING - Apple never polls ACTIVE after WAKE in PATH-2!
    ctx().WriteControlSet(kContextControlWakeBit);

    ASFW_LOG_KV(Async, RoleTag::kContextName.data(), txid, generation_, "P2_WAKE pulsed");
    trace_.push({NowNs(), txid, generation_, ATEvent::P2_WAKE, 0, 0});

    // Ring update under lock
    {
        IOLockWrapper lockWrapper(lock());
        ScopedLock guard(lockWrapper);
        UpdateRingTail_(chain);

        // FIX: Don't call requestStop_ with lock held - causes deadlock!
        // Apple's pattern: Let interrupt handler (ScanCompletion) stop context when ring drains
        // The interrupt handler already has correct stop logic at ATContextBase.hpp:886-903
        // if (opts.needsFlush) {
        //     requestStop_(txid, "needsFlush");
        // }
    }

    return kIOReturnSuccess;
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::RequestStop(uint32_t txid, const char* why) noexcept {
    // Phase 1.2: Use ScopedLock for automatic RAII
    IOLockWrapper lockWrapper(lock());
    ScopedLock guard(lockWrapper);
    requestStop_(txid, why);
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::requestStop_(uint32_t txid, const char* why) noexcept {
    if (this->state_ != State::RUNNING) {
        ASFW_LOG_KV(Async, RoleTag::kContextName.data(), txid, generation_, "STOP_SKIP state=%{public}s", ToString(this->state_));
        return;
    }

    Base::Transition(State::STOPPING, txid, why);
    const auto t0 = NowUs();

    ctx().WriteControlClear(kContextControlRunBit);
    IODelay(1);
    this->IoReadFence();

    // FIX: Don't poll for ACTIVE=0 with lock held - causes deadlock!
    // Apple's pattern: Fire-and-forget, interrupt handler detects quiescence
    // Polling here blocks interrupt handler from acquiring lock to drain completions
    // Result: Hardware ACTIVE never clears because completion isn't drained → deadlock
    // for (uint32_t i = 0; i < 250; ++i) {
    //     if (!ctx().IsActive()) break;
    //     IODelay(1);
    // }

    const auto elapsed = NowUs() - t0;
    
    // Verify ring is empty before rotation
    if (ring().Head() != ring().Tail()) {
        ASFW_LOG_ERROR(Async, "[%{public}s] STOP: Ring not empty (head=%zu tail=%zu)",
                       RoleTag::kContextName.data(), ring().Head(), ring().Tail());
    }

    rotateRingBy2_();
    ring().SetPrevLastBlocks(0);
    ++generation_;

    ASFW_LOG_KV(Async, RoleTag::kContextName.data(), txid, generation_, "STOP_IMM why=%{public}s elapsed_us=%lu gen=%u", why, (unsigned long)elapsed, generation_);
    trace_.push({NowNs(), txid, generation_, ATEvent::STOP_IMM, static_cast<uint32_t>(elapsed), generation_});

    Base::Transition(State::IDLE, txid, "stopped");
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::clearRunAndPoll_() noexcept {
    ctx().WriteControlClear(kContextControlRunBit);
    IODelay(1);
    this->IoReadFence();
    
    for (uint32_t i = 0; i < 250; ++i) {
        if (!ctx().IsActive()) break;
        IODelay(1);
    }
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::rotateRingBy2_() noexcept {
    const size_t capacity = ring().Capacity();
    if (capacity == 0) return;
    
    const size_t currentHead = ring().Head();
    const size_t newHead = (currentHead + 2) % capacity;
    ring().SetHead(newHead);
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::PublishChain_(const DescriptorChain& chain) {
    builder_.FlushChain(chain);
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::UpdateRingTail_(const DescriptorChain& chain) {
    const size_t newTail = (chain.lastRingIndex + 1) % ring().Capacity();
    ring().SetTail(newTail);
    // PrevLastBlocks is intended to track the block count of the LAST descriptor
    // in the previous chain. Use lastBlocks (1 or 2), not total packet blocks.
    ring().SetPrevLastBlocks(static_cast<uint8_t>(chain.lastBlocks));
}

template<typename ContextT, typename RingT, typename RoleTag>
kern_return_t ATManager<ContextT, RingT, RoleTag>::LinkTailTo_(const DescriptorChain& chain) {
    return builder_.LinkTailTo(ring().Tail(), chain) ? kIOReturnSuccess : kIOReturnNotReady;
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::UnlinkTail_() noexcept {
    builder_.UnlinkTail(ring().Tail());
}

template<typename ContextT, typename RingT, typename RoleTag>
void ATManager<ContextT, RingT, RoleTag>::PublishPrevLast_(const DescriptorChain& chain) {
    const uint8_t prevBlocks = ring().PrevLastBlocks();
    const size_t tailIndex = ring().Tail();
    
    // Locate previous LAST descriptor to determine if it's immediate
    HW::OHCIDescriptor* prevLast = nullptr;
    size_t prevLastIndex = 0;
    uint8_t blocks = 0;
    bool prevIsImmediate = false;
    
    if (ring().LocatePreviousLast(tailIndex, prevLast, prevLastIndex, blocks)) {
        prevIsImmediate = HW::IsImmediate(*prevLast);
        // Use the actual number of blocks for the previous LAST descriptor
        builder_.FlushTail(prevLastIndex, blocks);
        return;
    }

    // Fallback: flush using cached prevBlocks (should not typically happen)
    builder_.FlushTail(prevLastIndex, static_cast<uint8_t>(prevBlocks));
}

} // namespace ASFW::Async::Engine
