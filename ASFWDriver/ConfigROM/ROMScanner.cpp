#include "ROMScanner.hpp"
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../IRM/IRMTypes.hpp"
#include "ConfigROMStore.hpp"
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"

#include <array>
#include <cstring>

// #include <libkern/OSByteOrder.h>  // For OSSwapHostToBigInt32

namespace ASFW::Discovery {

ROMScanner::ROMScanner(Async::IFireWireBus& bus,
                       SpeedPolicy& speedPolicy,
                       ScanCompletionCallback onScanComplete,
                       OSSharedPtr<IODispatchQueue> dispatchQueue)
    : bus_(bus)
    , speedPolicy_(speedPolicy)
    , params_{.startSpeed = FwSpeed::S100, .maxInflight = 2, .perStepRetries = 2}
    , reader_(std::make_unique<ROMReader>(bus, dispatchQueue))
    , onScanComplete_(onScanComplete) {
}

ROMScanner::~ROMScanner() = default;

void ROMScanner::SetCompletionCallback(ScanCompletionCallback callback) {
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
        ASFW_LOG_V2(ConfigROM, "  Queue node %u for scanning", node.nodeId);
    }
    
    ASFW_LOG_V2(ConfigROM, "ROM Scanner: %zu remote nodes queued, starting scan...",
             nodeScans_.size());
    
    // Handle zero remote nodes case (single-node bus)
    if (nodeScans_.empty()) {
        ASFW_LOG_V2(ConfigROM, "ROM Scanner: No remote nodes — discovery complete for gen=%u", gen);
        // Call completion callback immediately for single-node bus (Apple pattern)
        if (onScanComplete_) {
            ASFW_LOG_V2(ConfigROM, "✅ ROMScanner: Single-node bus, notifying completion for gen=%u", gen);
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
        ASFW_LOG_V2(ConfigROM, "ROM Scanner: ABORT gen=%u (inflight=%u queued=%zu)",
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

        if (node.state == NodeState::Idle && !node.bibInProgress) {
            // Start BIB read
            node.state = NodeState::ReadingBIB;
            node.bibInProgress = true;
            inflightCount_++;

            ASFW_LOG_V2(ConfigROM, "FSM: Node %u → ReadingBIB (speed=S%u00 retries=%u)",
                     node.nodeId, static_cast<uint32_t>(node.currentSpeed) + 1,
                     node.retriesLeft);
            
            auto callback = [this, nodeId = node.nodeId](const ROMReader::ReadResult& result) {
                this->OnBIBComplete(nodeId, result);
            };

            reader_->ReadBIB(node.nodeId, currentGen_, node.currentSpeed, callback);
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
        ASFW_LOG(ConfigROM, "FSM: Node %u BIB read FAILED - marking as failed", nodeId);
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
        ASFW_LOG(ConfigROM, "FSM: Node %u BIB parse FAILED", nodeId);
        node.state = NodeState::Failed;
        // CRITICAL FIX: Don't call AdvanceFSM() from callback - causes re-entry deadlock
        // AdvanceFSM();
        // Check if scan complete (Apple pattern: fNumROMReads--)
        CheckAndNotifyCompletion();
        return;
    }
    
    node.partialROM.bib = bibOpt.value();

    // Seed raw quadlets vector with the Bus Info Block so ExportConfigROM always has data
    const uint32_t totalROMBytes = ROMParser::CalculateROMSize(node.partialROM.bib);
    const uint32_t totalROMQuadlets = totalROMBytes / 4;
    const uint32_t bibQuadlets = result.dataLength / 4;

    node.partialROM.rawQuadlets.clear();
    node.partialROM.rawQuadlets.reserve(totalROMQuadlets);
    if (result.data && bibQuadlets > 0) {
        for (uint32_t i = 0; i < bibQuadlets; ++i) {
            node.partialROM.rawQuadlets.push_back(result.data[i]);
        }
    }

    ASFW_LOG_V2(ConfigROM, "ROM size from BIB: %u bytes (%u quadlets)",
             totalROMBytes, totalROMQuadlets);

    speedPolicy_.RecordSuccess(nodeId, node.currentSpeed);

    node.needsIRMCheck = true;
    
    if (node.needsIRMCheck) {
        ASFW_LOG_V2(ConfigROM, "FSM: Node %u → VerifyingIRM_Read", nodeId);
        node.state = NodeState::VerifyingIRM_Read;
        inflightCount_++;

        auto callback = [this, nodeId](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            ROMReader::ReadResult mockResult{};
            mockResult.success = (status == Async::AsyncStatus::kSuccess);
            if (mockResult.success && payload.size() >= 4) {
                mockResult.dataLength = 4;
                mockResult.data = reinterpret_cast<const uint32_t*>(payload.data());
            }
            this->OnIRMReadComplete(nodeId, mockResult);
        };

        Async::FWAddress addr(IRM::IRMRegisters::kAddressHi,
                             IRM::IRMRegisters::kChannelsAvailable63_32,
                             (currentTopology_.busNumber.value_or(0) << 10) | nodeId);
        bus_.ReadQuad(FW::Generation(currentGen_), FW::NodeId(nodeId), addr,
                     FW::FwSpeed::S100, callback);
        return;
    }

    ASFW_LOG_V2(ConfigROM, "FSM: Node %u → ReadingRootDir", nodeId);
    node.state = NodeState::ReadingRootDir;
    node.retriesLeft = params_.perStepRetries;
    inflightCount_++;

    const uint32_t offsetBytes = 20;
    const uint32_t remainingBytes = totalROMBytes - 20;
    const uint32_t maxQuadlets = remainingBytes / 4;

    auto callback = [this, nodeId](const ROMReader::ReadResult& res) {
        this->OnRootDirComplete(nodeId, res);
    };

    reader_->ReadRootDirQuadlets(nodeId, currentGen_, node.currentSpeed,
                                 offsetBytes, maxQuadlets, callback);
}

void ROMScanner::OnIRMReadComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;

    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                          [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });

    if (it == nodeScans_.end()) {
        CheckAndNotifyCompletion();
        return;
    }

    auto& node = *it;
    node.bibInProgress = false;

    if (!result.success || !result.data) {
        ASFW_LOG_V2(ConfigROM, "⚠️  Node %u IRM read test FAILED - marking as bad IRM", nodeId);
        node.irmIsBad = true;

        if (topologyManager_ && currentTopology_.irmNodeId.has_value() &&
            *currentTopology_.irmNodeId == nodeId) {
            topologyManager_->MarkNodeAsBadIRM(nodeId);
            ASFW_LOG(ConfigROM, "  Current IRM failed verification - will trigger root reassignment");
        }

        node.state = NodeState::ReadingRootDir;
        node.retriesLeft = params_.perStepRetries;
        inflightCount_++;

        const uint32_t totalROMBytes = ROMParser::CalculateROMSize(node.partialROM.bib);
        const uint32_t offsetBytes = 20;
        const uint32_t remainingBytes = totalROMBytes - 20;
        const uint32_t maxQuadlets = remainingBytes / 4;

        auto callback = [this, nodeId](const ROMReader::ReadResult& res) {
            this->OnRootDirComplete(nodeId, res);
        };

        reader_->ReadRootDirQuadlets(nodeId, currentGen_, node.currentSpeed,
                                     offsetBytes, maxQuadlets, callback);
        return;
    }

    node.irmBitBucket = OSSwapBigToHostInt32(*result.data);
    node.irmCheckReadDone = true;

    ASFW_LOG_V2(ConfigROM, "FSM: Node %u IRM read test OK → VerifyingIRM_Lock", nodeId);
    node.state = NodeState::VerifyingIRM_Lock;
    inflightCount_++;

    auto callback = [this, nodeId](Async::AsyncStatus status, std::span<const uint8_t> payload) {
        ROMReader::ReadResult mockResult{};
        mockResult.success = (status == Async::AsyncStatus::kSuccess);
        if (mockResult.success && payload.size() >= 4) {
            mockResult.dataLength = 4;
            mockResult.data = reinterpret_cast<const uint32_t*>(payload.data());
        }
        this->OnIRMLockComplete(nodeId, mockResult);
    };

    Async::FWAddress addr(IRM::IRMRegisters::kAddressHi,
                         IRM::IRMRegisters::kChannelsAvailable63_32,
                         (currentTopology_.busNumber.value_or(0) << 10) | nodeId);

    std::array<uint8_t, 8> casOperand{};
    const uint32_t beCompare = OSSwapHostToBigInt32(0xFFFFFFFFu);
    const uint32_t beSwap = OSSwapHostToBigInt32(0xFFFFFFFFu);
    std::memcpy(casOperand.data(), &beCompare, sizeof(beCompare));
    std::memcpy(casOperand.data() + 4, &beSwap, sizeof(beSwap));

    auto handle = bus_.Lock(FW::Generation(currentGen_),
                            FW::NodeId(nodeId),
                            addr,
                            FW::LockOp::kCompareSwap,
                            std::span<const uint8_t>(casOperand),
                            4,
                            FW::FwSpeed::S100,
                            callback);

    if (!handle) {
        ASFW_LOG(ConfigROM, "⚠️  Node %u IRM lock submission failed", nodeId);
        inflightCount_--;

        ROMReader::ReadResult failure{};
        failure.success = false;

        HandleIRMLockResult(node, failure);
        return;
    }
}

void ROMScanner::OnIRMLockComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;

    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                          [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });

    if (it == nodeScans_.end()) {
        CheckAndNotifyCompletion();
        return;
    }

    HandleIRMLockResult(*it, result);
}

void ROMScanner::HandleIRMLockResult(NodeScanState& node, const ROMReader::ReadResult& result) {
    const uint8_t nodeId = node.nodeId;

    if (!result.success || !result.data) {
        ASFW_LOG(ConfigROM, "⚠️  Node %u IRM lock test FAILED - marking as bad IRM", nodeId);
        node.irmIsBad = true;

        if (topologyManager_ && currentTopology_.irmNodeId.has_value() &&
            *currentTopology_.irmNodeId == nodeId) {
            topologyManager_->MarkNodeAsBadIRM(nodeId);
            ASFW_LOG(ConfigROM, "  Current IRM failed verification - will trigger root reassignment");
        }
    } else {
        const uint32_t returnedValue = OSSwapBigToHostInt32(*result.data);
        node.irmCheckLockDone = true;

        ASFW_LOG_V2(ConfigROM, "✅ Node %u IRM verification PASSED (read=0x%08x lock=0x%08x)",
                 nodeId, node.irmBitBucket, returnedValue);
    }

    ASFW_LOG_V2(ConfigROM, "FSM: Node %u → ReadingRootDir", nodeId);
    node.state = NodeState::ReadingRootDir;
    node.retriesLeft = params_.perStepRetries;
    inflightCount_++;

    const uint32_t totalROMBytes = ROMParser::CalculateROMSize(node.partialROM.bib);
    const uint32_t offsetBytes = 20;
    const uint32_t remainingBytes = totalROMBytes - 20;
    const uint32_t maxQuadlets = remainingBytes / 4;

    auto callback = [this, nodeId](const ROMReader::ReadResult& res) {
        this->OnRootDirComplete(nodeId, res);
    };

    reader_->ReadRootDirQuadlets(nodeId, currentGen_, node.currentSpeed,
                                 offsetBytes, maxQuadlets, callback);
}

void ROMScanner::OnRootDirComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;

    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                          [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });

    if (it == nodeScans_.end()) {
        CheckAndNotifyCompletion();
        return;
    }

    auto& node = *it;

    if (!result.success) {
        ASFW_LOG(ConfigROM, "FSM: Node %u RootDir read FAILED - marking as failed", nodeId);
        node.state = NodeState::Failed;
        CheckAndNotifyCompletion();
        return;
    }
    
    const uint32_t quadletCount = result.dataLength / 4;
    auto entries = ROMParser::ParseRootDirectory(result.data, quadletCount);

    node.partialROM.rootDirMinimal = std::move(entries);

    if (result.data && result.dataLength > 0) {
        node.partialROM.rawQuadlets.reserve(quadletCount);
        for (uint32_t i = 0; i < quadletCount; ++i) {
            node.partialROM.rawQuadlets.push_back(result.data[i]);
        }
    }

    ASFW_LOG_V3(ConfigROM, "Text descriptor parsing: have %zu raw quadlets", node.partialROM.rawQuadlets.size());

    for (const auto& entry : node.partialROM.rootDirMinimal) {
        ASFW_LOG_V3(ConfigROM, "  Checking entry: key=0x%02x entryType=%u leafOffset=%u",
                 static_cast<uint8_t>(entry.key), entry.entryType, entry.leafOffsetQuadlets);

        if (entry.key == CfgKey::TextDescriptor && entry.entryType == EntryType::kLeaf) {
            constexpr uint32_t kBIBQuadlets = 5;
            const uint32_t absoluteROMOffset = kBIBQuadlets + entry.leafOffsetQuadlets;

            ASFW_LOG_V3(ConfigROM, "  → Attempting to parse text descriptor at root-dir-rel=%u absolute-ROM=%u",
                     entry.leafOffsetQuadlets, absoluteROMOffset);

            std::string text = ROMParser::ParseTextDescriptorLeaf(
                node.partialROM.rawQuadlets.data(),
                static_cast<uint32_t>(node.partialROM.rawQuadlets.size()),
                absoluteROMOffset,
                "big"
            );

            ASFW_LOG_V3(ConfigROM, "  → ParseTextDescriptorLeaf returned: '%{public}s' (length=%zu)",
                     text.c_str(), text.length());

            if (!text.empty()) {
                if (node.partialROM.vendorName.empty()) {
                    node.partialROM.vendorName = text;
                    ASFW_LOG_V1(ConfigROM, "✅ Parsed vendor name: %{public}s", text.c_str());
                } else if (node.partialROM.modelName.empty()) {
                    node.partialROM.modelName = text;
                    ASFW_LOG_V1(ConfigROM, "✅ Parsed model name: %{public}s", text.c_str());
                }
            }
        }
    }

    speedPolicy_.RecordSuccess(nodeId, node.currentSpeed);

    node.state = NodeState::Complete;
    completedROMs_.push_back(std::move(node.partialROM));

    ASFW_LOG_V2(ConfigROM, "FSM: Node %u → Complete ✓ (total complete=%zu)",
             nodeId, completedROMs_.size());

    CheckAndNotifyCompletion();
}

void ROMScanner::RetryWithFallback(NodeScanState& node) {
    if (node.retriesLeft > 0) {
        node.retriesLeft--;
        node.state = NodeState::Idle;
        ASFW_LOG_V2(ConfigROM, "FSM: Node %u retry at S%u00 (retries left=%u)",
                 node.nodeId, static_cast<uint32_t>(node.currentSpeed) + 1,
                 node.retriesLeft);
    } else {
        speedPolicy_.RecordTimeout(node.nodeId, node.currentSpeed);
        
        FwSpeed newSpeed = speedPolicy_.ForNode(node.nodeId).localToNode;
        if (newSpeed != node.currentSpeed) {
            const FwSpeed oldSpeed = node.currentSpeed;
            node.currentSpeed = newSpeed;
            node.retriesLeft = params_.perStepRetries;
            node.state = NodeState::Idle;
            ASFW_LOG_V2(ConfigROM, "FSM: Node %u speed fallback S%u00 → S%u00, retries reset",
                     node.nodeId,
                     static_cast<uint32_t>(oldSpeed) + 1,
                     static_cast<uint32_t>(newSpeed) + 1);
        } else {
            node.state = NodeState::Failed;
            ASFW_LOG(ConfigROM, "FSM: Node %u → Failed ✗ (exhausted retries)",
                     node.nodeId);
        }
    }
}

bool ROMScanner::HasCapacity() const {
    return inflightCount_ < params_.maxInflight;
}

void ROMScanner::CheckAndNotifyCompletion() {
    ASFW_LOG_V3(ConfigROM, "🔍 CheckAndNotifyCompletion: currentGen=%u nodeCount=%zu inflight=%u",
             currentGen_, nodeScans_.size(), inflightCount_);

    if (currentGen_ == 0) {
        ASFW_LOG_V3(ConfigROM, "  ⏭️  Not scanning (currentGen=0)");
        return;
    }

    if (nodeScans_.empty()) {
        ASFW_LOG_V3(ConfigROM, "  ⏭️  No nodes to scan (empty scan list)");
        return;
    }

    if (inflightCount_ > 0) {
        ASFW_LOG_V3(ConfigROM, "  ⏭️  Still have %u in-flight operations", inflightCount_);
        return;
    }

    for (const auto& node : nodeScans_) {
        if (node.state != NodeState::Complete && node.state != NodeState::Failed) {
            ASFW_LOG_V3(ConfigROM, "  ⏭️  Node %u still pending (state=%u)",
                     node.nodeId, static_cast<uint8_t>(node.state));
            return;
        }
    }

    if (onScanComplete_) {
        ASFW_LOG_V1(ConfigROM, "✅ ROMScanner: Scan complete for gen=%u, notifying immediately", currentGen_);
        onScanComplete_(currentGen_);
    } else {
        ASFW_LOG(ConfigROM, "⚠️  ROMScanner: Scan complete for gen=%u but NO callback set!", currentGen_);
    }
}

bool ROMScanner::TriggerManualRead(uint8_t nodeId, Generation gen, const Driver::TopologySnapshot& topology) {
    // If scanner is idle (currentGen_ == 0), we need to reinitialize it with the current generation
    // This happens after automatic scan completes and scanner marks itself idle
    if (currentGen_ == 0 && gen != 0) {
        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: scanner idle, restarting with gen=%u for node=%u",
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
        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: gen mismatch (requested=%u current=%u)",
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

        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: added node %u to scan list", nodeId);
    }

    // Check if already in progress
    if (nodeState->state == NodeState::ReadingBIB ||
        nodeState->state == NodeState::ReadingRootDir) {
        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: node %u already in progress", nodeId);
        return false;
    }

    // Check if already completed successfully
    if (nodeState->state == NodeState::Complete) {
        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: node %u already completed, restarting", nodeId);
    }

    // Reset node state to trigger a fresh read
    nodeState->state = NodeState::Idle;
    nodeState->currentSpeed = params_.startSpeed;
    nodeState->retriesLeft = params_.perStepRetries;
    nodeState->partialROM = ConfigROM{};
    nodeState->partialROM.gen = gen;
    nodeState->partialROM.nodeId = nodeId;

    ASFW_LOG_V2(ConfigROM, "TriggerManualRead: initiating ROM read for node %u gen=%u",
             nodeId, gen);

    // Kick off the read
    AdvanceFSM();

    return true;
}

} // namespace ASFW::Discovery
