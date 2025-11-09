#include "ROMScanner.hpp"
#include "../Async/AsyncSubsystem.hpp"
#include "ConfigROMStore.hpp"
#include "../Logging/Logging.hpp"

namespace ASFW::Discovery {

ROMScanner::ROMScanner(Async::AsyncSubsystem& asyncSubsystem,
                       SpeedPolicy& speedPolicy,
                       ScanCompletionCallback onScanComplete)
    : async_(asyncSubsystem)
    , speedPolicy_(speedPolicy)
    , params_{.startSpeed = FwSpeed::S100, .maxInflight = 2, .perStepRetries = 2}
    , reader_(std::make_unique<ROMReader>(asyncSubsystem))
    , onScanComplete_(onScanComplete) {
}

ROMScanner::~ROMScanner() = default;

void ROMScanner::SetCompletionCallback(ScanCompletionCallback callback) {
    onScanComplete_ = callback;
}

void ROMScanner::Begin(Generation gen,
                       const Driver::TopologySnapshot& topology,
                       uint8_t localNodeId) {
    // Abort any previous scan
    if (currentGen_ != 0) {
        Abort(currentGen_);
    }
    
    ASFW_LOG(Discovery, "══════════════════════════════════════════════");
    ASFW_LOG(Discovery, "ROM Scanner: Begin gen=%u localNode=%u topology nodes=%zu bus=%u",
             gen, localNodeId, topology.nodes.size(), topology.busNumber.value_or(0));
    
    currentGen_ = gen;
    currentTopology_ = topology;  // Store snapshot for bus info access
    nodeScans_.clear();
    completedROMs_.clear();
    inflightCount_ = 0;
    
    // Build worklist from topology (exclude local node)
    for (const auto& node : topology.nodes) {
        if (node.nodeId == localNodeId) {
            continue;  // Skip ourselves
        }
        if (!node.linkActive) {
            continue;  // Skip inactive nodes
        }
        
        NodeScanState scan{};
        scan.nodeId = node.nodeId;
        scan.state = NodeState::Idle;
        scan.currentSpeed = params_.startSpeed;
        scan.retriesLeft = params_.perStepRetries;
        scan.partialROM.gen = gen;
        scan.partialROM.nodeId = node.nodeId;
        
        nodeScans_.push_back(scan);
        ASFW_LOG(Discovery, "  Queue node %u for scanning", node.nodeId);
    }
    
    ASFW_LOG(Discovery, "ROM Scanner: %zu remote nodes queued, starting scan...",
             nodeScans_.size());
    
    // Handle zero remote nodes case (single-node bus)
    if (nodeScans_.empty()) {
        ASFW_LOG(Discovery, "ROM Scanner: No remote nodes — discovery complete for gen=%u", gen);
        // Call completion callback immediately for single-node bus (Apple pattern)
        if (onScanComplete_) {
            ASFW_LOG(Discovery, "✅ ROMScanner: Single-node bus, notifying completion for gen=%u", gen);
            onScanComplete_(gen);
        }
        // Mark as idle immediately so PollDiscovery sees completion
        currentGen_ = 0; // Reset so IsIdleFor returns true
        return;
    }
    
    // Kick off initial batch
    AdvanceFSM();
}

bool ROMScanner::IsIdleFor(Generation gen) const {
    if (gen != currentGen_) {
        return true;  // Not our generation
    }
    
    // Handle empty scan case (no remote nodes)
    if (nodeScans_.empty()) {
        return true; // No nodes to scan = idle
    }
    
    if (inflightCount_ > 0) {
        return false;  // Still have in-flight operations
    }
    
    // Check if all nodes are in terminal state
    for (const auto& node : nodeScans_) {
        if (node.state != NodeState::Complete && node.state != NodeState::Failed) {
            return false;
        }
    }
    
    return true;
}

std::vector<ConfigROM> ROMScanner::DrainReady(Generation gen) {
    if (gen != currentGen_) {
        return {};
    }
    
    std::vector<ConfigROM> result;
    result.swap(completedROMs_);
    return result;
}

void ROMScanner::Abort(Generation gen) {
    if (gen == currentGen_) {
        ASFW_LOG(Discovery, "ROM Scanner: ABORT gen=%u (inflight=%u queued=%zu)",
                 gen, inflightCount_, nodeScans_.size());
        nodeScans_.clear();
        completedROMs_.clear();
        inflightCount_ = 0;
        currentGen_ = 0;
    }
}

void ROMScanner::AdvanceFSM() {
    // Kick off new reads if we have capacity
    for (auto& node : nodeScans_) {
        if (!HasCapacity()) {
            break;  // Hit concurrency limit
        }
        
        if (node.state == NodeState::Idle) {
            // Start BIB read
            node.state = NodeState::ReadingBIB;
            inflightCount_++;
            
            ASFW_LOG(Discovery, "FSM: Node %u → ReadingBIB (speed=S%u00 retries=%u)",
                     node.nodeId, static_cast<uint32_t>(node.currentSpeed) + 1,
                     node.retriesLeft);
            
            auto callback = [this, nodeId = node.nodeId](const ROMReader::ReadResult& result) {
                this->OnBIBComplete(nodeId, result);
            };
            
            reader_->ReadBIB(node.nodeId, currentGen_, node.currentSpeed, currentTopology_.busBase16, callback);
        }
    }
}

void ROMScanner::OnBIBComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;
    
    // Find node state
    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                          [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });

    if (it == nodeScans_.end()) {
        // Node not found (aborted?)
        // CRITICAL FIX: Don't call AdvanceFSM() from callback - causes re-entry deadlock
        // AdvanceFSM();
        // Check if scan complete (node was aborted but scan might be done)
        CheckAndNotifyCompletion();
        return;
    }

    auto& node = *it;

    if (!result.success) {
        // BIB read failed - mark as failed (don't retry from callback to avoid deadlock)
        ASFW_LOG(Discovery, "FSM: Node %u BIB read FAILED - marking as failed", nodeId);
        // CRITICAL FIX: Don't retry from callback - causes re-entry deadlock when
        // callback is invoked from WithTransaction (which holds lock), then retry
        // calls RegisterTx → Allocate → lock attempt → DEADLOCK
        // RetryWithFallback(node);
        // AdvanceFSM();
        node.state = NodeState::Failed;
        // Check if scan complete (Apple pattern: fNumROMReads--)
        CheckAndNotifyCompletion();
        return;
    }

    // Parse BIB
    auto bibOpt = ROMParser::ParseBIB(result.data);
    if (!bibOpt.has_value()) {
        ASFW_LOG(Discovery, "FSM: Node %u BIB parse FAILED", nodeId);
        node.state = NodeState::Failed;
        // CRITICAL FIX: Don't call AdvanceFSM() from callback - causes re-entry deadlock
        // AdvanceFSM();
        // Check if scan complete (Apple pattern: fNumROMReads--)
        CheckAndNotifyCompletion();
        return;
    }
    
    node.partialROM.bib = bibOpt.value();

    // Calculate actual ROM size from BIB crc_length field
    const uint32_t totalROMBytes = ROMParser::CalculateROMSize(node.partialROM.bib);
    const uint32_t totalROMQuadlets = totalROMBytes / 4;

    ASFW_LOG(Discovery, "ROM size from BIB: %u bytes (%u quadlets), will read full ROM",
             totalROMBytes, totalROMQuadlets);

    // Record successful BIB read
    speedPolicy_.RecordSuccess(nodeId, node.currentSpeed);

    // Move to root directory read
    ASFW_LOG(Discovery, "FSM: Node %u → ReadingRootDir (reading full ROM)", nodeId);
    node.state = NodeState::ReadingRootDir;
    node.retriesLeft = params_.perStepRetries;  // Reset retries for next step
    inflightCount_++;

    // Read entire ROM minus BIB (BIB is 20 bytes, already read)
    // This gives us root directory + all leaves in one read
    const uint32_t offsetBytes = 20;
    const uint32_t remainingBytes = totalROMBytes - 20;
    const uint32_t maxQuadlets = remainingBytes / 4;
    
    auto callback = [this, nodeId](const ROMReader::ReadResult& res) {
        this->OnRootDirComplete(nodeId, res);
    };
    
    reader_->ReadRootDirQuadlets(nodeId, currentGen_, node.currentSpeed, currentTopology_.busBase16,
                                 offsetBytes, maxQuadlets, callback);
}

void ROMScanner::OnRootDirComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;
    
    // Find node state
    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                          [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });

    if (it == nodeScans_.end()) {
        // CRITICAL FIX: Don't call AdvanceFSM() from callback - causes re-entry deadlock
        // AdvanceFSM();
        // Check if scan complete (node was aborted but scan might be done)
        CheckAndNotifyCompletion();
        return;
    }

    auto& node = *it;

    if (!result.success) {
        // Root dir read failed - mark as failed (don't retry from callback to avoid deadlock)
        ASFW_LOG(Discovery, "FSM: Node %u RootDir read FAILED - marking as failed", nodeId);
        // CRITICAL FIX: Don't retry from callback - causes re-entry deadlock
        // RetryWithFallback(node);
        // AdvanceFSM();
        node.state = NodeState::Failed;
        // Check if scan complete (Apple pattern: fNumROMReads--)
        CheckAndNotifyCompletion();
        return;
    }
    
    // Parse root directory
    const uint32_t quadletCount = result.dataLength / 4;
    auto entries = ROMParser::ParseRootDirectory(result.data, quadletCount);

    node.partialROM.rootDirMinimal = std::move(entries);

    // Store ALL raw quadlets (ROM size determined from BIB, already bounded to IEEE 1394 max)
    if (result.data && result.dataLength > 0) {
        node.partialROM.rawQuadlets.reserve(quadletCount);
        for (uint32_t i = 0; i < quadletCount; ++i) {
            node.partialROM.rawQuadlets.push_back(result.data[i]);
        }
    }

    // Parse text descriptors from ROM (vendor/model names)
    // We have raw quadlets stored - parse text descriptor leaves
    // Note: leafOffsetQuadlets in entries are relative to root directory start
    ASFW_LOG(Discovery, "Text descriptor parsing: have %zu raw quadlets", node.partialROM.rawQuadlets.size());

    for (const auto& entry : node.partialROM.rootDirMinimal) {
        ASFW_LOG(Discovery, "  Checking entry: key=0x%02x entryType=%u leafOffset=%u",
                 static_cast<uint8_t>(entry.key), entry.entryType, entry.leafOffsetQuadlets);

        if (entry.key == CfgKey::TextDescriptor && entry.entryType == EntryType::kLeaf) {
            ASFW_LOG(Discovery, "  → Attempting to parse text descriptor at offset %u", entry.leafOffsetQuadlets);

            // Parse text from leaf (assume little-endian for now - should detect from BIB)
            std::string text = ROMParser::ParseTextDescriptorLeaf(
                node.partialROM.rawQuadlets.data(),
                static_cast<uint32_t>(node.partialROM.rawQuadlets.size()),
                entry.leafOffsetQuadlets,
                "little"  // TODO: Track endianness from BIB parsing
            );

            ASFW_LOG(Discovery, "  → ParseTextDescriptorLeaf returned: '%{public}s' (length=%zu)",
                     text.c_str(), text.length());

            if (!text.empty()) {
                // First text descriptor is typically vendor, second is model
                if (node.partialROM.vendorName.empty()) {
                    node.partialROM.vendorName = text;
                    ASFW_LOG(Discovery, "✅ Parsed vendor name: %{public}s", text.c_str());
                } else if (node.partialROM.modelName.empty()) {
                    node.partialROM.modelName = text;
                    ASFW_LOG(Discovery, "✅ Parsed model name: %{public}s", text.c_str());
                }
            }
        }
    }

    // Record success
    speedPolicy_.RecordSuccess(nodeId, node.currentSpeed);

    // Move completed ROM to output queue
    node.state = NodeState::Complete;
    completedROMs_.push_back(std::move(node.partialROM));

    ASFW_LOG(Discovery, "FSM: Node %u → Complete ✓ (total complete=%zu)",
             nodeId, completedROMs_.size());

    // CRITICAL FIX: Don't call AdvanceFSM() from callback - causes re-entry deadlock
    // The FSM will be advanced externally when needed (e.g., on next manual trigger or bus reset)
    // AdvanceFSM();

    // Check if scan complete (Apple pattern: if(fNumROMReads == 0) finishedBusScan())
    CheckAndNotifyCompletion();
}

void ROMScanner::RetryWithFallback(NodeScanState& node) {
    if (node.retriesLeft > 0) {
        // Retry at current speed
        node.retriesLeft--;
        node.state = NodeState::Idle;  // Will be retried in next AdvanceFSM
        ASFW_LOG(Discovery, "FSM: Node %u retry at S%u00 (retries left=%u)",
                 node.nodeId, static_cast<uint32_t>(node.currentSpeed) + 1,
                 node.retriesLeft);
    } else {
        // Out of retries - try downgrading speed
        speedPolicy_.RecordTimeout(node.nodeId, node.currentSpeed);
        
        FwSpeed newSpeed = speedPolicy_.ForNode(node.nodeId).localToNode;
        if (newSpeed != node.currentSpeed) {
            // Speed downgraded, reset retries
            const FwSpeed oldSpeed = node.currentSpeed;
            node.currentSpeed = newSpeed;
            node.retriesLeft = params_.perStepRetries;
            node.state = NodeState::Idle;
            ASFW_LOG(Discovery, "FSM: Node %u speed fallback S%u00 → S%u00, retries reset",
                     node.nodeId,
                     static_cast<uint32_t>(oldSpeed) + 1,
                     static_cast<uint32_t>(newSpeed) + 1);
        } else {
            // Can't downgrade further - give up
            node.state = NodeState::Failed;
            ASFW_LOG(Discovery, "FSM: Node %u → Failed ✗ (exhausted retries)",
                     node.nodeId);
        }
    }
}

bool ROMScanner::HasCapacity() const {
    return inflightCount_ < params_.maxInflight;
}

// Apple-style immediate completion check (matches fNumROMReads-- pattern)
void ROMScanner::CheckAndNotifyCompletion() {
    ASFW_LOG(Discovery, "🔍 CheckAndNotifyCompletion: currentGen=%u nodeCount=%zu inflight=%u",
             currentGen_, nodeScans_.size(), inflightCount_);

    // Check if scanner is idle (matches IsIdleFor logic)
    if (currentGen_ == 0) {
        ASFW_LOG(Discovery, "  ⏭️  Not scanning (currentGen=0)");
        return;  // Not scanning
    }

    if (nodeScans_.empty()) {
        ASFW_LOG(Discovery, "  ⏭️  No nodes to scan (empty scan list)");
        return;  // No nodes to scan
    }

    if (inflightCount_ > 0) {
        ASFW_LOG(Discovery, "  ⏭️  Still have %u in-flight operations", inflightCount_);
        return;  // Still have in-flight operations
    }

    // Check if all nodes are in terminal state
    for (const auto& node : nodeScans_) {
        if (node.state != NodeState::Complete && node.state != NodeState::Failed) {
            ASFW_LOG(Discovery, "  ⏭️  Node %u still pending (state=%u)",
                     node.nodeId, static_cast<uint8_t>(node.state));
            return;  // Some nodes still pending
        }
    }

    // All nodes complete! Notify immediately (Apple pattern: if(fNumROMReads == 0) finishedBusScan())
    if (onScanComplete_) {
        ASFW_LOG(Discovery, "✅ ROMScanner: Scan complete for gen=%u, notifying immediately (Apple pattern)", currentGen_);
        onScanComplete_(currentGen_);
    } else {
        ASFW_LOG(Discovery, "⚠️  ROMScanner: Scan complete for gen=%u but NO callback set!", currentGen_);
    }
}

bool ROMScanner::TriggerManualRead(uint8_t nodeId, Generation gen, const Driver::TopologySnapshot& topology) {
    // If scanner is idle (currentGen_ == 0), we need to reinitialize it with the current generation
    // This happens after automatic scan completes and scanner marks itself idle
    if (currentGen_ == 0 && gen != 0) {
        ASFW_LOG(Discovery, "TriggerManualRead: scanner idle, restarting with gen=%u for node=%u",
                 gen, nodeId);
        // Set generation and prepare for manual scan
        currentGen_ = gen;
        currentTopology_ = topology;  // Update topology to get correct busBase16
        nodeScans_.clear();
        completedROMs_.clear();
        inflightCount_ = 0;
    }
    // Validate generation matches current scan
    else if (gen != currentGen_) {
        ASFW_LOG(Discovery, "TriggerManualRead: gen mismatch (requested=%u current=%u)",
                 gen, currentGen_);
        return false;
    }

    // Find the node in our scan list
    NodeScanState* nodeState = nullptr;
    for (auto& node : nodeScans_) {
        if (node.nodeId == nodeId) {
            nodeState = &node;
            break;
        }
    }

    // If node not in our list, add it
    if (!nodeState) {
        // UserClient already validated node exists in topology, so we can skip that check
        // when scanner was just restarted (currentTopology_ may be stale)

        // Add new node to scan list
        NodeScanState newNode{};
        newNode.nodeId = nodeId;
        newNode.state = NodeState::Idle;
        newNode.currentSpeed = params_.startSpeed;
        newNode.retriesLeft = params_.perStepRetries;
        newNode.partialROM.gen = gen;
        newNode.partialROM.nodeId = nodeId;

        nodeScans_.push_back(newNode);
        nodeState = &nodeScans_.back();

        ASFW_LOG(Discovery, "TriggerManualRead: added node %u to scan list", nodeId);
    }

    // Check if already in progress
    if (nodeState->state == NodeState::ReadingBIB ||
        nodeState->state == NodeState::ReadingRootDir) {
        ASFW_LOG(Discovery, "TriggerManualRead: node %u already in progress", nodeId);
        return false;
    }

    // Check if already completed successfully
    if (nodeState->state == NodeState::Complete) {
        ASFW_LOG(Discovery, "TriggerManualRead: node %u already completed, restarting", nodeId);
    }

    // Reset node state to trigger a fresh read
    nodeState->state = NodeState::Idle;
    nodeState->currentSpeed = params_.startSpeed;
    nodeState->retriesLeft = params_.perStepRetries;
    nodeState->partialROM = ConfigROM{};
    nodeState->partialROM.gen = gen;
    nodeState->partialROM.nodeId = nodeId;

    ASFW_LOG(Discovery, "TriggerManualRead: initiating ROM read for node %u gen=%u",
             nodeId, gen);

    // Kick off the read
    AdvanceFSM();

    return true;
}

} // namespace ASFW::Discovery

