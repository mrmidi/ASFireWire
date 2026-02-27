#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "ROMScanNodeStateMachine.hpp"
#include "ROMScannerCompletionManager.hpp"
#include "ROMScannerEventBus.hpp"
#include "ROMScannerInflightCoordinator.hpp"
#include "ROMReader.hpp"
#include "SpeedPolicy.hpp"

namespace ASFW::Discovery {

class ROMScannerFSMController {
public:
    void AdvanceFSM(
        std::vector<ROMScanNodeStateMachine>& nodeScans,
        ROMReader& reader,
        Generation currentGen,
        uint16_t inflightCount,
        uint16_t maxInflight,
        const std::function<bool(ROMScanNodeStateMachine&, ROMScanNodeStateMachine::State, const char*)>& transitionNodeState,
        const std::function<void()>& incrementInflight,
        const std::function<void(ROMScannerEventType, uint8_t, const ROMReader::ReadResult&)>& publishReadEvent) const;

    void RetryWithFallback(
        ROMScanNodeStateMachine& node,
        SpeedPolicy& speedPolicy,
        uint8_t perStepRetries,
        const std::function<bool(ROMScanNodeStateMachine&, ROMScanNodeStateMachine::State, const char*)>& transitionNodeState) const;

    [[nodiscard]] bool HasCapacity(const ROMScannerInflightCoordinator& inflight,
                                   uint16_t maxInflight) const;

    void CheckAndNotifyCompletion(
        Generation currentGen,
        const std::vector<ROMScanNodeStateMachine>& nodeScans,
        uint16_t inflightCount,
        ROMScannerCompletionManager& completionMgr,
        const std::function<void(Generation)>& onScanComplete) const;
};

} // namespace ASFW::Discovery
