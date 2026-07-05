#pragma once

// SBP2BridgeHub — process-global hand-off point between the main FireWire
// service (ASFWDriver, which owns the SBP-2 stack) and the HBA service
// (ASFWSCSIController). Both run in the same dext process (same
// IOUserServerName), but the HBA's provider chain goes through the SCSI
// kernel companion, so it cannot reach the ASFWSBP2Nub object the way
// ASFWAudioDriver casts its provider. This hub is the in-process rendezvous:
// DriverContext publishes the bridge after the SBP-2 stack is wired, and the
// HBA fetches a shared_ptr per task (so a concurrent teardown cannot free the
// bridge under it — Shutdown() flips the bridge to fail-fast instead).

#include <memory>

namespace ASFW::Protocols::SBP2 {

class SBP2TargetBridge;

class SBP2BridgeHub {
public:
    static void Set(std::shared_ptr<SBP2TargetBridge> bridge);
    static void Clear();
    [[nodiscard]] static std::shared_ptr<SBP2TargetBridge> Get();
};

} // namespace ASFW::Protocols::SBP2
