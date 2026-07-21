#pragma once

// SBP2BridgeHub — process-global hand-off point between the main FireWire
// service (ASFWDriver, which owns the SBP-2 stack) and the HBA service
// (ASFWSCSIController). Both run in the same dext process (same
// IOUserServerName), but the HBA's provider chain goes through the SCSI
// kernel companion, so it cannot reach ASFWDriver-owned objects the way
// ASFWAudioDriver casts its provider. This hub is the in-process rendezvous:
// DriverContext publishes the bridge after the SBP-2 stack is wired, and the
// HBA fetches a shared_ptr per task (so a concurrent teardown cannot free the
// bridge under it — Shutdown() flips the bridge to fail-fast instead).

#include <cstdint>
#include <functional>
#include <memory>

namespace ASFW::Protocols::SBP2 {

class SBP2TargetBridge;

class SBP2BridgeHub {
public:
    static void Set(std::shared_ptr<SBP2TargetBridge> bridge);
    static void Clear();
    [[nodiscard]] static std::shared_ptr<SBP2TargetBridge> Get();

    // Reverse channel: the HBA (a separate IOService, unreachable via the
    // provider chain) registers a target-state observer so the FireWire side can
    // drive SCSI target create/destroy on SBP-2 login/logout. loggedIn=true on
    // login up, false on logout/failure. The HBA registers on Start and clears
    // on Stop. NotifyTargetState fires the observer UNDER the hub lock — the
    // HBA's Stop is load-bearing on that synchrony (once ClearTargetObserver
    // returns, no observer call is running or can start), so the observer must
    // only schedule non-blocking work; see NotifyTargetState.
    using TargetStateCallback = std::function<void(uint64_t guid, bool loggedIn)>;
    static void SetTargetObserver(TargetStateCallback observer);
    static void ClearTargetObserver();
    static void NotifyTargetState(uint64_t guid, bool loggedIn);
};

} // namespace ASFW::Protocols::SBP2
