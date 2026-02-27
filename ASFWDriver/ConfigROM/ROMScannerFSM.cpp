#include "ROMScanner.hpp"
#include "ROMScannerEnsurePrefixController.hpp"
#include "ROMScannerFSMController.hpp"
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>

namespace ASFW::Discovery {

bool ROMScanner::TransitionNodeState(ROMScanNodeStateMachine& node,
                                     NodeState next,
                                     const char* reason) const {
    if (node.TransitionTo(next)) {
        return true;
    }

    ASFW_LOG(ConfigROM,
             "FSM: invalid node state transition node=%u from=%u to=%u (%s)",
             node.NodeId(),
             static_cast<uint8_t>(node.CurrentState()),
             static_cast<uint8_t>(next),
             reason ? reason : "unspecified");
    node.ForceState(NodeState::Failed);
    return false;
}

void ROMScanner::AdvanceFSM() {
    fsmController_.AdvanceFSM(
        nodeScans_,
        *reader_,
        currentGen_,
        InflightCount(),
        params_.maxInflight,
        [this](ROMScanNodeStateMachine& node,
               ROMScanNodeStateMachine::State next,
               const char* reason) {
            return this->TransitionNodeState(node, next, reason);
        },
        [this]() {
            this->IncrementInflight();
        },
        [this](ROMScannerEventType type,
               uint8_t nodeId,
               const ROMReader::ReadResult& result) {
            this->PublishReadEvent(type, nodeId, result);
        });
}

void ROMScanner::RetryWithFallback(ROMScanNodeStateMachine& node) {
    fsmController_.RetryWithFallback(
        node,
        speedPolicy_,
        params_.perStepRetries,
        [this](ROMScanNodeStateMachine& node,
               ROMScanNodeStateMachine::State next,
               const char* reason) {
            return this->TransitionNodeState(node, next, reason);
        });
}

ROMScanNodeStateMachine* ROMScanner::FindNodeScan(uint8_t nodeId) {
    auto it = std::ranges::find_if(nodeScans_,
                                   [nodeId](const ROMScanNodeStateMachine& n) { return n.NodeId() == nodeId; });
    return (it != nodeScans_.end()) ? std::to_address(it) : nullptr;
}

bool ROMScanner::HasCapacity() const {
    return fsmController_.HasCapacity(inflight_, params_.maxInflight);
}

void ROMScanner::DispatchAsync(void (^work)()) {
    if (!dispatchQueue_) {
        if (work) {
            work();
        }
        return;
    }

    auto queue = dispatchQueue_;
    queue->DispatchAsync(work);
}

void ROMScanner::ScheduleAdvanceFSM() {
    // Never call AdvanceFSM() directly from async completion callbacks.
    DispatchAsync(^{
        this->AdvanceFSM();
    });
}

void ROMScanner::EnsurePrefix(uint8_t nodeId,
                              uint32_t requiredTotalQuadlets,
                              std::function<void(bool)> completion) {
    ensurePrefixController_.EnsurePrefix(
        nodeId,
        requiredTotalQuadlets,
        currentGen_,
        *reader_,
        [this](uint8_t nodeId) {
            return this->FindNodeScan(nodeId);
        },
        [this]() {
            this->IncrementInflight();
        },
        [this](uint8_t nodeId,
               uint32_t requiredTotalQuadlets,
               const std::function<void(bool)>& completion,
               const ROMReader::ReadResult& result) {
            this->PublishEnsurePrefixEvent(nodeId,
                                           requiredTotalQuadlets,
                                           completion,
                                           result);
        },
        std::move(completion));
}

void ROMScanner::CheckAndNotifyCompletion() {
    fsmController_.CheckAndNotifyCompletion(currentGen_,
                                            nodeScans_,
                                            InflightCount(),
                                            completionMgr_,
                                            onScanComplete_);
}

} // namespace ASFW::Discovery
