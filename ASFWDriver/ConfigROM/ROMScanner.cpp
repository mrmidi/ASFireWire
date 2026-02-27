#include "ROMScanner.hpp"
#include "ROMScannerFSMFlow.hpp"
#include "ConfigROMPolicies.hpp"
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"

#include <algorithm>

namespace ASFW::Discovery {

ROMScanner::ROMScanner(Async::IFireWireBus& bus,
                       SpeedPolicy& speedPolicy,
                       const ROMScannerParams& params,
                       const ScanCompletionCallback& onScanComplete,
                       OSSharedPtr<IODispatchQueue> dispatchQueue)
    : bus_(bus)
    , speedPolicy_(speedPolicy)
    , params_(params)
    , reader_(std::make_unique<ROMReader>(bus, dispatchQueue))
    , dispatchQueue_(dispatchQueue)
    , onScanComplete_(onScanComplete) {
}

ROMScanner::~ROMScanner() = default;

void ROMScanner::PublishReadEvent(ROMScannerEventType type,
                                  uint8_t nodeId,
                                  const ROMReader::ReadResult& result) {
    ROMScannerEvent event{};
    event.type = type;
    event.payload = ROMScannerReadEventData::FromReadResult(nodeId, result);
    eventBus_.Publish(std::move(event));
    ScheduleEventDrain();
}

void ROMScanner::PublishEnsurePrefixEvent(uint8_t nodeId,
                                          uint32_t requiredTotalQuadlets,
                                          const std::function<void(bool)>& completion,
                                          const ROMReader::ReadResult& result) {
    ROMScannerEvent event{};
    event.type = ROMScannerEventType::EnsurePrefixComplete;
    event.payload = ROMScannerReadEventData::FromReadResult(nodeId, result);
    event.requiredTotalQuadlets = requiredTotalQuadlets;
    if (completion) {
        event.ensurePrefixCompletion = std::make_shared<std::function<void(bool)>>(completion);
    }
    eventBus_.Publish(std::move(event));
    ScheduleEventDrain();
}

bool ROMScanner::IsCurrentGenerationEvent(const ROMScannerReadEventData& payload) const {
    return GenerationContextPolicy::IsCurrentEvent(payload.generation, currentGen_);
}

void ROMScanner::ScheduleEventDrain() {
    DispatchAsync(^{
        this->ProcessPendingEvents();
    });
}

void ROMScanner::ProcessPendingEvents() {
    eventBus_.Drain([this](const ROMScannerEvent& event) {
        if (!IsCurrentGenerationEvent(event.payload)) {
            return;
        }

        auto result = event.payload.ToReadResult();
        switch (event.type) {
            case ROMScannerEventType::BIBComplete:
                OnBIBComplete(event.payload.nodeId, result);
                break;
            case ROMScannerEventType::IRMReadComplete:
                OnIRMReadComplete(event.payload.nodeId, result);
                break;
            case ROMScannerEventType::IRMLockComplete:
                OnIRMLockComplete(event.payload.nodeId, result);
                break;
            case ROMScannerEventType::RootDirComplete:
                OnRootDirComplete(event.payload.nodeId, result);
                break;
            case ROMScannerEventType::EnsurePrefixComplete:
                ROMScannerFSMFlow::OnEnsurePrefixReadComplete(*this,
                                                              event.payload.nodeId,
                                                              event.requiredTotalQuadlets,
                                                              event.ensurePrefixCompletion,
                                                              result);
                break;
        }
    });
}

void ROMScanner::SetCompletionCallback(const ScanCompletionCallback& callback) {
    onScanComplete_ = callback;
}

void ROMScanner::SetTopologyManager(Driver::TopologyManager* topologyManager) {
    topologyManager_ = topologyManager;
}

void ROMScanner::Begin(Generation gen,
                       const Driver::TopologySnapshot& topology,
                       uint8_t localNodeId) {
    // Abort any previous scan
    if (currentGen_ != 0) {
        Abort(currentGen_);
    }
    
    ASFW_LOG_V2(ConfigROM, "══════════════════════════════════════════════");
    ASFW_LOG_V2(ConfigROM, "ROM Scanner: Begin gen=%u localNode=%u topology nodes=%zu bus=%u",
             gen, localNodeId, topology.nodes.size(), topology.busNumber.value_or(0));
    
    currentGen_ = gen;
    ResetCompletionNotification();
    hadBusyNodes_ = false;
    eventBus_.Clear();
    currentTopology_ = topology;  // Store snapshot for bus info access
    nodeScans_.clear();
    completedROMs_.clear();
    ResetInflight();
    
    // Build worklist from topology (exclude local node)
    for (const auto& node : topology.nodes) {
        if (node.nodeId == localNodeId) {
            continue;  // Skip ourselves
        }
        if (!node.linkActive) {
            continue;  // Skip inactive nodes
        }
        
        nodeScans_.emplace_back(node.nodeId, gen, params_.startSpeed, params_.perStepRetries);
        ASFW_LOG_V2(ConfigROM, "  Queue node %u for scanning", node.nodeId);
    }
    
    ASFW_LOG_V2(ConfigROM, "ROM Scanner: %zu remote nodes queued, starting scan...",
             nodeScans_.size());
    
    // Handle zero remote nodes case (single-node bus)
    if (nodeScans_.empty()) {
        ASFW_LOG_V2(ConfigROM, "ROM Scanner: No remote nodes — discovery complete for gen=%u", gen);
        MarkCompletionNotified();
        // Call completion callback immediately for single-node bus (Apple pattern)
        if (onScanComplete_) {
            ASFW_LOG_V2(ConfigROM, "✅ ROMScanner: Single-node bus, notifying completion for gen=%u", gen);
            onScanComplete_(gen);
        }
        return;
    }
    
    // Kick off initial batch
    AdvanceFSM();
}

bool ROMScanner::IsIdleFor(Generation gen) const {
    if (!GenerationContextPolicy::MatchesActiveScan(gen, currentGen_)) {
        return true;  // Not our generation
    }
    
    // Handle empty scan case (no remote nodes)
    if (nodeScans_.empty()) {
        return true; // No nodes to scan = idle
    }
    
    if (InflightCount() > 0) {
        return false;  // Still have in-flight operations
    }
    
    return std::ranges::all_of(nodeScans_, [](const ROMScanNodeStateMachine& node) {
        return node.IsTerminal();
    });
}

std::vector<ConfigROM> ROMScanner::DrainReady(Generation gen) {
    if (!GenerationContextPolicy::MatchesActiveScan(gen, currentGen_)) {
        return {};
    }
    
    std::vector<ConfigROM> result;
    result.swap(completedROMs_);
    return result;
}

void ROMScanner::Abort(Generation gen) {
    if (GenerationContextPolicy::MatchesActiveScan(gen, currentGen_)) {
        ASFW_LOG_V2(ConfigROM, "ROM Scanner: ABORT gen=%u (inflight=%u queued=%zu)",
                 gen, InflightCount(), nodeScans_.size());
        nodeScans_.clear();
        completedROMs_.clear();
        ResetInflight();
        currentGen_ = 0;
        ResetCompletionNotification();
        eventBus_.Clear();
    }
}

bool ROMScanner::TriggerManualRead(uint8_t nodeId, Generation gen, const Driver::TopologySnapshot& topology) {
    // If scanner is idle for a previous generation (or never started), we can restart
    // it for this manual read generation.
    if (const bool scannerIdle = (currentGen_ == 0) ? true : IsIdleFor(currentGen_);
        GenerationContextPolicy::CanRestartIdleScan(currentGen_, scannerIdle, gen)) {
        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: restarting idle scan (oldGen=%u → gen=%u) for node=%u",
                    currentGen_, gen, nodeId);

        currentGen_ = gen;
        ResetCompletionNotification();
        hadBusyNodes_ = false;
        eventBus_.Clear();
        currentTopology_ = topology;  // Update topology to get correct busBase16
        nodeScans_.clear();
        completedROMs_.clear();
        ResetInflight();
    } else if (!GenerationContextPolicy::MatchesActiveScan(gen, currentGen_)) {
        // Active scan: generation must match.
        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: gen mismatch (requested=%u current=%u)",
                    gen, currentGen_);
        return false;
    }

    // Find the node in our scan list
    ROMScanNodeStateMachine* nodeState = FindNodeScan(nodeId);

    // If node not in our list, add it
    if (!nodeState) {
        // UserClient already validated node exists in topology, so we can skip that check
        // when scanner was just restarted (currentTopology_ may be stale)

        // Add new node to scan list
        nodeScans_.emplace_back(nodeId, gen, params_.startSpeed, params_.perStepRetries);
        nodeState = &nodeScans_.back();

        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: added node %u to scan list", nodeId);
    }

    // Check if already in progress
    if (nodeState->CurrentState() == NodeState::ReadingBIB ||
        nodeState->CurrentState() == NodeState::ReadingRootDir) {
        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: node %u already in progress", nodeId);
        return false;
    }

    // Check if already completed successfully
    if (nodeState->CurrentState() == NodeState::Complete) {
        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: node %u already completed, restarting", nodeId);
    }

    // Reset node state to trigger a fresh read
    nodeState->ResetForGeneration(gen, nodeId, params_.startSpeed, params_.perStepRetries);

    ASFW_LOG_V2(ConfigROM, "TriggerManualRead: initiating ROM read for node %u gen=%u",
             nodeId, gen);

    // Kick off the read
    AdvanceFSM();

    return true;
}

void ROMScanner::IncrementInflight() {
    inflight_.Increment();
}

void ROMScanner::DecrementInflight() {
    inflight_.Decrement();
}

void ROMScanner::ResetInflight() {
    inflight_.Reset();
}

uint16_t ROMScanner::InflightCount() const {
    return inflight_.Count();
}

void ROMScanner::ResetCompletionNotification() {
    completionMgr_.Reset();
}

void ROMScanner::MarkCompletionNotified() {
    completionMgr_.MarkNotified();
}

bool ROMScanner::TryMarkCompletionNotified() {
    return completionMgr_.TryMarkNotified();
}

} // namespace ASFW::Discovery
