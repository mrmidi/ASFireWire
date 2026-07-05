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

} // namespace ASFW::Protocols::SBP2
