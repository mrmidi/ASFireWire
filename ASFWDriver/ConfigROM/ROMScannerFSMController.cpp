#include "ROMScannerFSMController.hpp"
#include "ConfigROMPolicies.hpp"

#include "../Logging/LogConfig.hpp"
#include "../Logging/Logging.hpp"

#include <algorithm>

namespace ASFW::Discovery {

void ROMScannerFSMController::AdvanceFSM(
    std::vector<ROMScanNodeStateMachine>& nodeScans,
    ROMReader& reader,
    Generation currentGen,
    uint16_t inflightCount,
    uint16_t maxInflight,
    const std::function<bool(ROMScanNodeStateMachine&, ROMScanNodeStateMachine::State, const char*)>& transitionNodeState,
    const std::function<void()>& incrementInflight,
    const std::function<void(ROMScannerEventType, uint8_t, const ROMReader::ReadResult&)>& publishReadEvent) const {
    for (auto& node : nodeScans) {
        if (inflightCount >= maxInflight) {
            break;
        }

        if (node.CurrentState() == ROMScanNodeStateMachine::State::Idle && !node.BIBInProgress()) {
            if (!transitionNodeState(node,
                                     ROMScanNodeStateMachine::State::ReadingBIB,
                                     "AdvanceFSM start BIB")) {
                continue;
            }
            node.SetBIBInProgress(true);
            incrementInflight();
            ++inflightCount;

            ASFW_LOG_V2(ConfigROM,
                        "FSM: Node %u -> ReadingBIB (speed=S%u00 retries=%u)",
                        node.NodeId(),
                        static_cast<uint32_t>(node.CurrentSpeed()) + 1,
                        node.RetriesLeft());

            auto callback = [publishReadEvent, nodeId = node.NodeId()](const ROMReader::ReadResult& result) {
                publishReadEvent(ROMScannerEventType::BIBComplete, nodeId, result);
            };

            reader.ReadBIB(node.NodeId(), currentGen, node.CurrentSpeed(), callback);
        }
    }
}

void ROMScannerFSMController::RetryWithFallback(
    ROMScanNodeStateMachine& node,
    SpeedPolicy& speedPolicy,
    uint8_t perStepRetries,
    const std::function<bool(ROMScanNodeStateMachine&, ROMScanNodeStateMachine::State, const char*)>& transitionNodeState) const {
    const auto oldSpeed = node.CurrentSpeed();
    const RetryBackoffPolicy retryPolicy{};
    const auto decision = retryPolicy.Apply(node,
                                            speedPolicy,
                                            perStepRetries,
                                            transitionNodeState);

    switch (decision) {
        case RetryBackoffPolicy::Decision::RetrySameSpeed:
            ASFW_LOG_V2(ConfigROM,
                        "FSM: Node %u retry at S%u00 (retries left=%u)",
                        node.NodeId(),
                        static_cast<uint32_t>(node.CurrentSpeed()) + 1,
                        node.RetriesLeft());
            break;
        case RetryBackoffPolicy::Decision::RetryWithFallback:
            ASFW_LOG_V2(ConfigROM,
                        "FSM: Node %u speed fallback S%u00 -> S%u00, retries reset",
                        node.NodeId(),
                        static_cast<uint32_t>(oldSpeed) + 1,
                        static_cast<uint32_t>(node.CurrentSpeed()) + 1);
            break;
        case RetryBackoffPolicy::Decision::FailedExhausted:
            ASFW_LOG(ConfigROM,
                     "FSM: Node %u -> Failed (exhausted retries)",
                     node.NodeId());
            break;
    }
}

bool ROMScannerFSMController::HasCapacity(const ROMScannerInflightCoordinator& inflight,
                                          uint16_t maxInflight) const {
    return inflight.HasCapacity(maxInflight);
}

void ROMScannerFSMController::CheckAndNotifyCompletion(
    Generation currentGen,
    const std::vector<ROMScanNodeStateMachine>& nodeScans,
    uint16_t inflightCount,
    ROMScannerCompletionManager& completionMgr,
    const std::function<void(Generation)>& onScanComplete) const {
    ASFW_LOG_V3(ConfigROM,
                "CheckAndNotifyCompletion: currentGen=%u nodeCount=%zu inflight=%u",
                currentGen,
                nodeScans.size(),
                inflightCount);

    if (currentGen == 0 || nodeScans.empty() || inflightCount > 0) {
        return;
    }

    const bool allTerminal = std::all_of(nodeScans.begin(),
                                         nodeScans.end(),
                                         [](const ROMScanNodeStateMachine& node) {
                                             return node.IsTerminal();
                                         });
    if (!allTerminal) {
        return;
    }

    if (!completionMgr.TryMarkNotified()) {
        return;
    }

    if (onScanComplete) {
        onScanComplete(currentGen);
    } else {
        ASFW_LOG(ConfigROM,
                 "ROMScanner: Scan complete for gen=%u but no callback set",
                 currentGen);
    }
}

} // namespace ASFW::Discovery
