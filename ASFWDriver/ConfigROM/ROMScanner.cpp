#include "ROMScanner.hpp"
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../IRM/IRMTypes.hpp"
#include "ConfigROMStore.hpp"
#include "../Logging/Logging.hpp"

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
    
    ASFW_LOG(ConfigROM, "══════════════════════════════════════════════");
    ASFW_LOG(ConfigROM, "ROM Scanner: Begin gen=%u localNode=%u topology nodes=%zu bus=%u",
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
        ASFW_LOG(ConfigROM, "  Queue node %u for scanning", node.nodeId);
    }
    
    ASFW_LOG(ConfigROM, "ROM Scanner: %zu remote nodes queued, starting scan...",
             nodeScans_.size());
    
    // Handle zero remote nodes case (single-node bus)
    if (nodeScans_.empty()) {
        ASFW_LOG(ConfigROM, "ROM Scanner: No remote nodes — discovery complete for gen=%u", gen);
        // Call completion callback immediately for single-node bus (Apple pattern)
        if (onScanComplete_) {
            ASFW_LOG(ConfigROM, "✅ ROMScanner: Single-node bus, notifying completion for gen=%u", gen);
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
        ASFW_LOG(ConfigROM, "ROM Scanner: ABORT gen=%u (inflight=%u queued=%zu)",
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

            ASFW_LOG(ConfigROM, "FSM: Node %u → ReadingBIB (speed=S%u00 retries=%u)",
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

    // Calculate actual ROM size from BIB crc_length field

    ASFW_LOG(ConfigROM, "ROM size from BIB: %u bytes (%u quadlets), will read full ROM",
             totalROMBytes, totalROMQuadlets);

    // Record successful BIB read
    speedPolicy_.RecordSuccess(nodeId, node.currentSpeed);

    // ========================================================================
    // Phase 3: IRM Capability Verification
    // ========================================================================
    // Check if node needs IRM verification (must be contender with speed > S100)
    // Reference: Apple IOFireWireController.cpp:2641-2697

    bool isContender = false;

    // Find node in topology to get contender status
    for (const auto& topoNode : currentTopology_.nodes) {
        if (topoNode.nodeId == nodeId) {
            isContender = topoNode.isIRMCandidate;
            break;
        }
    }

    // Always verify contenders regardless of BIB-reported link speed.
    // Apple performs the CSR read/CAS at S100 even when the device negotiates
    // faster links; some PHYs report linkSpeedCode=0 even when running s400.
    node.needsIRMCheck = false;
    
    if (node.needsIRMCheck) {
        // Begin IRM verification: Phase 1 = Read test
        ASFW_LOG(ConfigROM, "FSM: Node %u → VerifyingIRM_Read (contender verification)", nodeId);
        node.state = NodeState::VerifyingIRM_Read;
        inflightCount_++;

        // Read CHANNELS_AVAILABLE_63_32 register (0xF0000228)
        // This verifies node can respond to CSR reads
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

    // Skip IRM verification, move directly to root directory read
    ASFW_LOG(ConfigROM, "FSM: Node %u → ReadingRootDir (reading full ROM)", nodeId);
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

    reader_->ReadRootDirQuadlets(nodeId, currentGen_, node.currentSpeed,
                                 offsetBytes, maxQuadlets, callback);
}

// ============================================================================
// Phase 3: IRM Verification Handlers
// ============================================================================

void ROMScanner::OnIRMReadComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;

    // Find node state
    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                          [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });

    if (it == nodeScans_.end()) {
        CheckAndNotifyCompletion();
        return;
    }

    auto& node = *it;
    node.bibInProgress = false;

    if (!result.success || !result.data) {
        // Read test failed - mark node as bad IRM
        ASFW_LOG(ConfigROM, "⚠️  Node %u IRM read test FAILED - marking as bad IRM", nodeId);
        node.irmIsBad = true;

        if (topologyManager_ && currentTopology_.irmNodeId.has_value() &&
            *currentTopology_.irmNodeId == nodeId) {
            // This node is current IRM and failed verification
            topologyManager_->MarkNodeAsBadIRM(nodeId);
            ASFW_LOG(ConfigROM, "  Current IRM failed verification - will trigger root reassignment");
        }

        // Skip lock test, proceed to ROM reading
        node.state = NodeState::ReadingRootDir;
        node.retriesLeft = params_.perStepRetries;
        inflightCount_++;

        // Read root directory
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

    // Read succeeded, store value and move to lock test
    node.irmBitBucket = OSSwapBigToHostInt32(*result.data);
    node.irmCheckReadDone = true;

    ASFW_LOG(ConfigROM, "FSM: Node %u IRM read test OK → VerifyingIRM_Lock (CAS test)", nodeId);
    node.state = NodeState::VerifyingIRM_Lock;
    inflightCount_++;

    // Perform CAS test: compare=0xFFFFFFFF, swap=0xFFFFFFFF (no-op)
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
                            /*responseLength=*/4,
                            FW::FwSpeed::S100,
                            callback);

    if (!handle) {
        ASFW_LOG(ConfigROM, "⚠️  Node %u IRM lock submission failed", nodeId);
        inflightCount_--;  // Undo increment

        ROMReader::ReadResult failure{};
        failure.success = false;

        HandleIRMLockResult(node, failure);
        return;
    }
}

void ROMScanner::OnIRMLockComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;

    // Find node state
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
        // Lock test failed - mark node as bad IRM
        ASFW_LOG(ConfigROM, "⚠️  Node %u IRM lock test FAILED - marking as bad IRM", nodeId);
        node.irmIsBad = true;

        if (topologyManager_ && currentTopology_.irmNodeId.has_value() &&
            *currentTopology_.irmNodeId == nodeId) {
            // This node is current IRM and failed verification
            topologyManager_->MarkNodeAsBadIRM(nodeId);
            ASFW_LOG(ConfigROM, "  Current IRM failed verification - will trigger root reassignment");
        }
    } else {
        // Lock test succeeded
        const uint32_t returnedValue = OSSwapBigToHostInt32(*result.data);
        node.irmCheckLockDone = true;

        ASFW_LOG(ConfigROM, "✅ Node %u IRM verification PASSED (read=0x%08x lock=0x%08x)",
                 nodeId, node.irmBitBucket, returnedValue);
    }

    // Proceed to root directory read
    ASFW_LOG(ConfigROM, "FSM: Node %u → ReadingRootDir (reading full ROM)", nodeId);
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

// ============================================================================
// Root Directory Read Handler
// ============================================================================

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
        ASFW_LOG(ConfigROM, "FSM: Node %u RootDir read FAILED - marking as failed", nodeId);
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
    ASFW_LOG(ConfigROM, "Text descriptor parsing: have %zu raw quadlets", node.partialROM.rawQuadlets.size());

    for (const auto& entry : node.partialROM.rootDirMinimal) {
        ASFW_LOG(ConfigROM, "  Checking entry: key=0x%02x entryType=%u leafOffset=%u",
                 static_cast<uint8_t>(entry.key), entry.entryType, entry.leafOffsetQuadlets);

        if (entry.key == CfgKey::TextDescriptor && entry.entryType == EntryType::kLeaf) {
            // Per IEEE 1394-1995 §8.3: BIB block is 5 quadlets (offsets 0-4), root directory starts at offset 5
            // leafOffsetQuadlets is relative to root directory start, so add 5 to get absolute ROM offset
            constexpr uint32_t kBIBQuadlets = 5;
            const uint32_t absoluteROMOffset = kBIBQuadlets + entry.leafOffsetQuadlets;

            ASFW_LOG(ConfigROM, "  → Attempting to parse text descriptor at root-dir-rel=%u absolute-ROM=%u",
                     entry.leafOffsetQuadlets, absoluteROMOffset);

            // Parse text from leaf (endianness parameter unused - ParseTextDescriptorLeaf always uses big-endian per IEEE 1212)
            std::string text = ROMParser::ParseTextDescriptorLeaf(
                node.partialROM.rawQuadlets.data(),
                static_cast<uint32_t>(node.partialROM.rawQuadlets.size()),
                absoluteROMOffset,
                "big"  // IEEE 1212: Config ROM structure is always big-endian
            );

            ASFW_LOG(ConfigROM, "  → ParseTextDescriptorLeaf returned: '%{public}s' (length=%zu)",
                     text.c_str(), text.length());

            if (!text.empty()) {
                // First text descriptor is typically vendor, second is model
                if (node.partialROM.vendorName.empty()) {
                    node.partialROM.vendorName = text;
                    ASFW_LOG(ConfigROM, "✅ Parsed vendor name: %{public}s", text.c_str());
                } else if (node.partialROM.modelName.empty()) {
                    node.partialROM.modelName = text;
                    ASFW_LOG(ConfigROM, "✅ Parsed model name: %{public}s", text.c_str());
                }
            }
        }
    }

    // Record success
    speedPolicy_.RecordSuccess(nodeId, node.currentSpeed);

    // Move completed ROM to output queue
    node.state = NodeState::Complete;
    completedROMs_.push_back(std::move(node.partialROM));

    ASFW_LOG(ConfigROM, "FSM: Node %u → Complete ✓ (total complete=%zu)",
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
        ASFW_LOG(ConfigROM, "FSM: Node %u retry at S%u00 (retries left=%u)",
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
            ASFW_LOG(ConfigROM, "FSM: Node %u speed fallback S%u00 → S%u00, retries reset",
                     node.nodeId,
                     static_cast<uint32_t>(oldSpeed) + 1,
                     static_cast<uint32_t>(newSpeed) + 1);
        } else {
            // Can't downgrade further - give up
            node.state = NodeState::Failed;
            ASFW_LOG(ConfigROM, "FSM: Node %u → Failed ✗ (exhausted retries)",
                     node.nodeId);
        }
    }
}

bool ROMScanner::HasCapacity() const {
    return inflightCount_ < params_.maxInflight;
}

// Apple-style immediate completion check (matches fNumROMReads-- pattern)
void ROMScanner::CheckAndNotifyCompletion() {
    ASFW_LOG(ConfigROM, "🔍 CheckAndNotifyCompletion: currentGen=%u nodeCount=%zu inflight=%u",
             currentGen_, nodeScans_.size(), inflightCount_);

    // Check if scanner is idle (matches IsIdleFor logic)
    if (currentGen_ == 0) {
        ASFW_LOG(ConfigROM, "  ⏭️  Not scanning (currentGen=0)");
        return;  // Not scanning
    }

    if (nodeScans_.empty()) {
        ASFW_LOG(ConfigROM, "  ⏭️  No nodes to scan (empty scan list)");
        return;  // No nodes to scan
    }

    if (inflightCount_ > 0) {
        ASFW_LOG(ConfigROM, "  ⏭️  Still have %u in-flight operations", inflightCount_);
        return;  // Still have in-flight operations
    }

    // Check if all nodes are in terminal state
    for (const auto& node : nodeScans_) {
        if (node.state != NodeState::Complete && node.state != NodeState::Failed) {
            ASFW_LOG(ConfigROM, "  ⏭️  Node %u still pending (state=%u)",
                     node.nodeId, static_cast<uint8_t>(node.state));
            return;  // Some nodes still pending
        }
    }

    // All nodes complete! Notify immediately (Apple pattern: if(fNumROMReads == 0) finishedBusScan())
    if (onScanComplete_) {
        ASFW_LOG(ConfigROM, "✅ ROMScanner: Scan complete for gen=%u, notifying immediately (Apple pattern)", currentGen_);
        onScanComplete_(currentGen_);
    } else {
        ASFW_LOG(ConfigROM, "⚠️  ROMScanner: Scan complete for gen=%u but NO callback set!", currentGen_);
    }
}

bool ROMScanner::TriggerManualRead(uint8_t nodeId, Generation gen, const Driver::TopologySnapshot& topology) {
    // If scanner is idle (currentGen_ == 0), we need to reinitialize it with the current generation
    // This happens after automatic scan completes and scanner marks itself idle
    if (currentGen_ == 0 && gen != 0) {
        ASFW_LOG(ConfigROM, "TriggerManualRead: scanner idle, restarting with gen=%u for node=%u",
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
        ASFW_LOG(ConfigROM, "TriggerManualRead: gen mismatch (requested=%u current=%u)",
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

        ASFW_LOG(ConfigROM, "TriggerManualRead: added node %u to scan list", nodeId);
    }

    // Check if already in progress
    if (nodeState->state == NodeState::ReadingBIB ||
        nodeState->state == NodeState::ReadingRootDir) {
        ASFW_LOG(ConfigROM, "TriggerManualRead: node %u already in progress", nodeId);
        return false;
    }

    // Check if already completed successfully
    if (nodeState->state == NodeState::Complete) {
        ASFW_LOG(ConfigROM, "TriggerManualRead: node %u already completed, restarting", nodeId);
    }

    // Reset node state to trigger a fresh read
    nodeState->state = NodeState::Idle;
    nodeState->currentSpeed = params_.startSpeed;
    nodeState->retriesLeft = params_.perStepRetries;
    nodeState->partialROM = ConfigROM{};
    nodeState->partialROM.gen = gen;
    nodeState->partialROM.nodeId = nodeId;

    ASFW_LOG(ConfigROM, "TriggerManualRead: initiating ROM read for node %u gen=%u",
             nodeId, gen);

    // Kick off the read
    AdvanceFSM();

    return true;
}

} // namespace ASFW::Discovery
