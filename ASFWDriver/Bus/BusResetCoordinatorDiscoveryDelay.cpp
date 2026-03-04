#include "BusResetCoordinator.hpp"

#include <algorithm>

#include "Logging.hpp"

namespace ASFW::Driver {

bool BusResetCoordinator::ReadyForDiscovery(Discovery::Generation gen) const {
    const bool nodeValid = G_NodeIDValid();
    const bool genMatch = (gen == lastGeneration_);
    const bool hasTopo = lastTopology_.has_value();
    const bool ready = nodeValid && filtersEnabled_ && atArmed_ && hasTopo && genMatch;

    if (!ready) {
        ASFW_LOG(BusReset,
                 "ReadyForDiscovery(gen=%u): NOT READY — nodeValid=%d filters=%d at=%d "
                 "topo=%d genMatch=%d(last=%u)",
                 gen.value, nodeValid, filtersEnabled_, atArmed_, hasTopo, genMatch,
                 lastGeneration_.value);
    }
    return ready;
}

void BusResetCoordinator::SetPreviousScanHadBusyNodes(bool busy) {
    if (busy) {
        // Escalate: increase delay each consecutive busy scan
        if (currentDiscoveryDelayMs_ < kMaxDiscoveryDelayMs) {
            currentDiscoveryDelayMs_ =
                std::min(currentDiscoveryDelayMs_ + kDiscoveryDelayStepMs, kMaxDiscoveryDelayMs);
        }
        if (!previousScanHadBusyNodes_) { // NOSONAR(cpp:S3923): branches log different diagnostic
                                          // messages
            ASFW_LOG(BusReset, "previousScanHadBusyNodes: false → true, delay=%ums",
                     currentDiscoveryDelayMs_);
        } else {
            ASFW_LOG(BusReset, "previousScanHadBusyNodes: still true, delay escalated to %ums",
                     currentDiscoveryDelayMs_);
        }
    } else {
        // Device recovered — reset delay
        if (previousScanHadBusyNodes_ || currentDiscoveryDelayMs_ > 0) {
            ASFW_LOG(BusReset, "previousScanHadBusyNodes: %d → false, delay reset (was %ums)",
                     previousScanHadBusyNodes_, currentDiscoveryDelayMs_);
        }
        currentDiscoveryDelayMs_ = 0;
    }
    previousScanHadBusyNodes_ = busy;
}

void BusResetCoordinator::EscalateDiscoveryDelay() {
    // Called when scan produced 0 ROMs — we learned nothing, increase delay
    if (previousScanHadBusyNodes_ && currentDiscoveryDelayMs_ < kMaxDiscoveryDelayMs) {
        const uint32_t prev = currentDiscoveryDelayMs_;
        currentDiscoveryDelayMs_ =
            std::min(currentDiscoveryDelayMs_ + kDiscoveryDelayStepMs, kMaxDiscoveryDelayMs);
        ASFW_LOG(BusReset, "Discovery delay escalated %ums → %ums (0 ROMs, device still booting)",
                 prev, currentDiscoveryDelayMs_);
    }
}

} // namespace ASFW::Driver

