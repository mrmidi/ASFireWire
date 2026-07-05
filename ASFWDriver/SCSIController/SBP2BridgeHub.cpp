// SBP2BridgeHub — process-global bridge slot. See SBP2BridgeHub.hpp.

#include "SBP2BridgeHub.hpp"
#include "SBP2TargetBridge.hpp"

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#endif

namespace ASFW::Protocols::SBP2 {

namespace {

struct HubState {
    IOLock* lock{nullptr};
    std::shared_ptr<SBP2TargetBridge> bridge;
    SBP2BridgeHub::TargetStateCallback targetObserver;

    HubState() { lock = IOLockAlloc(); }
};

HubState& State() {
    static HubState state; // thread-safe magic-static init
    return state;
}

} // namespace

void SBP2BridgeHub::Set(std::shared_ptr<SBP2TargetBridge> bridge) {
    auto& s = State();
    IOLockLock(s.lock);
    s.bridge = std::move(bridge);
    IOLockUnlock(s.lock);
}

void SBP2BridgeHub::Clear() {
    // Drop the reference outside the lock (the bridge dtor takes its own lock).
    std::shared_ptr<SBP2TargetBridge> dropped;
    auto& s = State();
    IOLockLock(s.lock);
    dropped.swap(s.bridge);
    IOLockUnlock(s.lock);
}

std::shared_ptr<SBP2TargetBridge> SBP2BridgeHub::Get() {
    auto& s = State();
    IOLockLock(s.lock);
    auto bridge = s.bridge;
    IOLockUnlock(s.lock);
    return bridge;
}

void SBP2BridgeHub::SetTargetObserver(TargetStateCallback observer) {
    auto& s = State();
    IOLockLock(s.lock);
    s.targetObserver = std::move(observer);
    IOLockUnlock(s.lock);
}

void SBP2BridgeHub::ClearTargetObserver() {
    // Drop the observer outside the lock (it may own captured state with a
    // non-trivial destructor).
    TargetStateCallback dropped;
    auto& s = State();
    IOLockLock(s.lock);
    dropped.swap(s.targetObserver);
    IOLockUnlock(s.lock);
}

void SBP2BridgeHub::NotifyTargetState(uint64_t guid, bool loggedIn) {
    // Copy under the lock, invoke outside it: the observer hops onto the HBA's
    // own queue and must not run under the hub lock.
    TargetStateCallback observer;
    auto& s = State();
    IOLockLock(s.lock);
    observer = s.targetObserver;
    IOLockUnlock(s.lock);
    if (observer) {
        observer(guid, loggedIn);
    }
}

} // namespace ASFW::Protocols::SBP2
