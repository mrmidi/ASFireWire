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
    , dispatchQueue_(dispatchQueue)
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
    completionNotified_ = false;
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
        completionNotified_ = false;
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
        ScheduleAdvanceFSM();
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
        ScheduleAdvanceFSM();
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
        ScheduleAdvanceFSM();
        return;
    }
    
    node.partialROM.bib = bibOpt.value();

    // Seed raw quadlets vector with the Bus Info Block so ExportConfigROM always has data
    const uint32_t bibQuadlets = result.dataLength / 4;

    node.partialROM.rawQuadlets.clear();
    node.partialROM.rawQuadlets.reserve(256); // Bounded prefix: max 1 KiB / 256 quadlets
    if (result.data && bibQuadlets > 0) {
        for (uint32_t i = 0; i < bibQuadlets; ++i) {
            node.partialROM.rawQuadlets.push_back(result.data[i]);
        }
    }

    ASFW_LOG_V2(ConfigROM, "Seeded ROM prefix with BIB: %u quadlets (rootDirStart=%u)",
                bibQuadlets, RootDirStartQuadlet(node.partialROM.bib));

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
        ScheduleAdvanceFSM();
        return;
    }

    ASFW_LOG_V2(ConfigROM, "FSM: Node %u → ReadingRootDir", nodeId);
    node.state = NodeState::ReadingRootDir;
    node.retriesLeft = params_.perStepRetries;
    inflightCount_++;

    const uint32_t offsetBytes = RootDirStartBytes(node.partialROM.bib);

    auto callback = [this, nodeId](const ROMReader::ReadResult& res) {
        this->OnRootDirComplete(nodeId, res);
    };

    reader_->ReadRootDirQuadlets(nodeId, currentGen_, node.currentSpeed,
                                 offsetBytes, 0, callback); // header-first autosize
    ScheduleAdvanceFSM();
}

void ROMScanner::OnIRMReadComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;

    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                          [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });

    if (it == nodeScans_.end()) {
        CheckAndNotifyCompletion();
        ScheduleAdvanceFSM();
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

        const uint32_t offsetBytes = RootDirStartBytes(node.partialROM.bib);

        auto callback = [this, nodeId](const ROMReader::ReadResult& res) {
            this->OnRootDirComplete(nodeId, res);
        };

        reader_->ReadRootDirQuadlets(nodeId, currentGen_, node.currentSpeed,
                                     offsetBytes, 0, callback); // header-first autosize
        ScheduleAdvanceFSM();
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

    ScheduleAdvanceFSM();
}

void ROMScanner::OnIRMLockComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;

    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                          [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });

    if (it == nodeScans_.end()) {
        CheckAndNotifyCompletion();
        ScheduleAdvanceFSM();
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

    const uint32_t offsetBytes = RootDirStartBytes(node.partialROM.bib);

    auto callback = [this, nodeId](const ROMReader::ReadResult& res) {
        this->OnRootDirComplete(nodeId, res);
    };

    reader_->ReadRootDirQuadlets(nodeId, currentGen_, node.currentSpeed,
                                 offsetBytes, 0, callback); // header-first autosize
    ScheduleAdvanceFSM();
}

void ROMScanner::OnRootDirComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    inflightCount_--;

    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                          [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });

    if (it == nodeScans_.end()) {
        CheckAndNotifyCompletion();
        ScheduleAdvanceFSM();
        return;
    }

    auto& node = *it;

    if (!result.success || !result.data || result.dataLength < 4) {
        ASFW_LOG(ConfigROM, "FSM: Node %u RootDir read FAILED - marking as failed", nodeId);
        node.state = NodeState::Failed;
        CheckAndNotifyCompletion();
        ScheduleAdvanceFSM();
        return;
    }
    
    const uint32_t quadletCount = result.dataLength / 4;

    // Parse root directory (bounded).
    node.partialROM.rootDirMinimal = ROMParser::ParseRootDirectory(result.data, quadletCount);

    // Copy root directory quadlets (wire order) out of the ROMReader buffer, since it will be freed.
    std::vector<uint32_t> rootDirBE;
    rootDirBE.reserve(quadletCount);
    for (uint32_t i = 0; i < quadletCount; ++i) {
        rootDirBE.push_back(result.data[i]);
    }

    // Ensure rawQuadlets is a contiguous prefix from quadlet 0 and includes the root directory.
    const uint32_t rootDirStart = RootDirStartQuadlet(node.partialROM.bib);

    // Move the node into a "details" state while we fetch leaves/directories.
    node.state = NodeState::ReadingDetails;

    EnsurePrefix(nodeId, rootDirStart, [this, nodeId, rootDirStart, rootDirBE = std::move(rootDirBE)](bool prefixOk) mutable {
        // Re-find node (vector may have moved).
        auto it2 = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
        if (it2 == nodeScans_.end()) {
            CheckAndNotifyCompletion();
            ScheduleAdvanceFSM();
            return;
        }

        auto& node2 = *it2;

        if (!prefixOk) {
            ASFW_LOG(ConfigROM, "⚠️  Node %u: ROM prefix could not be extended to rootDirStart=%u", nodeId, rootDirStart);
        }

        if (node2.partialROM.rawQuadlets.size() != rootDirStart) {
            ASFW_LOG(ConfigROM, "⚠️  Node %u: ROM prefix size mismatch (have=%zu expected=%u) before root dir append",
                     nodeId, node2.partialROM.rawQuadlets.size(), rootDirStart);
        }

        node2.partialROM.rawQuadlets.reserve(node2.partialROM.rawQuadlets.size() + rootDirBE.size());
        for (uint32_t q : rootDirBE) {
            node2.partialROM.rawQuadlets.push_back(q);
        }

        struct DirEntry {
            uint32_t index{0};   // 1-based within directory
            uint8_t keyType{0};
            uint8_t keyId{0};
            uint32_t value{0};   // 24-bit
            bool hasTarget{false};
            uint32_t targetRel{0}; // quadlets relative to directory header
        };

        auto signExtend24 = [](uint32_t v24) -> int32_t {
            return (v24 & 0x00800000u) ? static_cast<int32_t>(v24 | 0xFF000000u) : static_cast<int32_t>(v24);
        };

        auto parseDirFromBE = [&](const std::vector<uint32_t>& dirBE, uint32_t entryCap) -> std::vector<DirEntry> {
            std::vector<DirEntry> out;
            if (dirBE.empty()) return out;
            const uint32_t hdr = ROMParser::SwapBE32(dirBE[0]);
            const uint32_t len = (hdr >> 16) & 0xFFFFu;
            const uint32_t available = (dirBE.size() > 0) ? static_cast<uint32_t>(dirBE.size() - 1) : 0;
            uint32_t count = len;
            if (count > available) count = available;
            if (count > entryCap) count = entryCap;

            out.reserve(count);
            for (uint32_t i = 1; i <= count; ++i) {
                const uint32_t entry = ROMParser::SwapBE32(dirBE[i]);
                DirEntry e{};
                e.index = i;
                e.keyType = static_cast<uint8_t>((entry >> 30) & 0x3u);
                e.keyId = static_cast<uint8_t>((entry >> 24) & 0x3Fu);
                e.value = entry & 0x00FFFFFFu;

                if (e.keyType == ASFW::FW::EntryType::kLeaf || e.keyType == ASFW::FW::EntryType::kDirectory) {
                    const int32_t off = signExtend24(e.value);
                    const int32_t rel = static_cast<int32_t>(i) + off;
                    if (rel >= 0) {
                        e.hasTarget = true;
                        e.targetRel = static_cast<uint32_t>(rel);
                    }
                }
                out.push_back(e);
            }
            return out;
        };

        const auto rootEntries = parseDirFromBE(rootDirBE, 64);

        struct DescriptorRef {
            uint8_t keyType{0};     // leaf or directory
            uint32_t targetRel{0};  // relative to the directory containing the entry
        };

        auto findAssociatedDescriptor = [&](uint8_t ownerKeyId) -> std::optional<DescriptorRef> {
            for (size_t i = 0; i < rootEntries.size(); ++i) {
                const auto& e = rootEntries[i];
                if (e.keyType != ASFW::FW::EntryType::kImmediate || e.keyId != ownerKeyId) {
                    continue;
                }
                if (i + 1 >= rootEntries.size()) {
                    return std::nullopt;
                }

                const auto& d = rootEntries[i + 1];
                if (d.keyId != ASFW::FW::ConfigKey::kTextualDescriptor) {
                    return std::nullopt;
                }
                if (d.keyType != ASFW::FW::EntryType::kLeaf && d.keyType != ASFW::FW::EntryType::kDirectory) {
                    return std::nullopt;
                }
                if (!d.hasTarget || d.targetRel == 0) {
                    return std::nullopt;
                }
                return DescriptorRef{.keyType = d.keyType, .targetRel = d.targetRel};
            }
            return std::nullopt;
        };

        const auto vendorDesc = findAssociatedDescriptor(ASFW::FW::ConfigKey::kModuleVendorId);
        const auto modelDesc = findAssociatedDescriptor(ASFW::FW::ConfigKey::kModelId);

        std::vector<uint32_t> unitDirRelOffsets;
        for (const auto& e : rootEntries) {
            if (e.keyType == ASFW::FW::EntryType::kDirectory &&
                e.keyId == ASFW::FW::ConfigKey::kUnitDirectory &&
                e.hasTarget && e.targetRel != 0) {
                unitDirRelOffsets.push_back(e.targetRel);
            }
        }

        // Async helpers: read leaf/dir headers + contents as needed and parse minimal ASCII text leaves.
        auto fetchTextLeafAt = std::make_shared<std::function<void(uint32_t, std::function<void(std::string)>)>>();
        auto fetchDescriptorDirText = std::make_shared<std::function<void(uint32_t, std::function<void(std::string)>)>>();

        *fetchTextLeafAt = [this, nodeId, fetchTextLeafAt](uint32_t absLeafOffset, std::function<void(std::string)> done) {
            // Ensure leaf header is present.
            this->EnsurePrefix(nodeId, absLeafOffset + 1, [this, nodeId, absLeafOffset, done = std::move(done)](bool ok) mutable {
                if (!ok) {
                    if (done) done("");
                    return;
                }

                auto it3 = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                        [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
                if (it3 == nodeScans_.end()) {
                    if (done) done("");
                    return;
                }

                auto& node3 = *it3;
                if (absLeafOffset >= node3.partialROM.rawQuadlets.size()) {
                    if (done) done("");
                    return;
                }

                const uint32_t hdr = ROMParser::SwapBE32(node3.partialROM.rawQuadlets[absLeafOffset]);
                const uint16_t leafLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);
                const uint32_t leafEndExclusive = absLeafOffset + 1u + static_cast<uint32_t>(leafLen);

                this->EnsurePrefix(nodeId, leafEndExclusive, [this, nodeId, absLeafOffset, done = std::move(done)](bool ok2) mutable {
                    if (!ok2) {
                        if (done) done("");
                        return;
                    }

                    auto it4 = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                            [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
                    if (it4 == nodeScans_.end()) {
                        if (done) done("");
                        return;
                    }

                    auto& node4 = *it4;
                    std::string text = ROMParser::ParseTextDescriptorLeaf(
                        node4.partialROM.rawQuadlets.data(),
                        static_cast<uint32_t>(node4.partialROM.rawQuadlets.size()),
                        absLeafOffset,
                        "big");
                    if (done) done(std::move(text));
                });
            });
        };

        *fetchDescriptorDirText = [this, nodeId, fetchTextLeafAt, fetchDescriptorDirText](uint32_t absDirOffset, std::function<void(std::string)> done) {
            // Ensure directory header is present.
            this->EnsurePrefix(nodeId, absDirOffset + 1, [this, nodeId, absDirOffset, fetchTextLeafAt, fetchDescriptorDirText, done = std::move(done)](bool ok) mutable {
                if (!ok) {
                    if (done) done("");
                    return;
                }

                auto it3 = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                        [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
                if (it3 == nodeScans_.end()) {
                    if (done) done("");
                    return;
                }

                auto& node3 = *it3;
                if (absDirOffset >= node3.partialROM.rawQuadlets.size()) {
                    if (done) done("");
                    return;
                }

                const uint32_t hdr = ROMParser::SwapBE32(node3.partialROM.rawQuadlets[absDirOffset]);
                uint16_t dirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);
                if (dirLen == 0) {
                    if (done) done("");
                    return;
                }

                if (dirLen > 32) {
                    dirLen = 32;
                }

                const uint32_t dirEndExclusive = absDirOffset + 1u + static_cast<uint32_t>(dirLen);
                this->EnsurePrefix(nodeId, dirEndExclusive, [this, nodeId, absDirOffset, dirLen, fetchTextLeafAt, fetchDescriptorDirText, done = std::move(done)](bool ok2) mutable {
                    if (!ok2) {
                        if (done) done("");
                        return;
                    }

                    auto it4 = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                            [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
                    if (it4 == nodeScans_.end()) {
                        if (done) done("");
                        return;
                    }

                    auto& node4 = *it4;
                    if (absDirOffset + dirLen >= node4.partialROM.rawQuadlets.size()) {
                        if (done) done("");
                        return;
                    }

                    struct Candidate {
                        uint32_t absLeaf{0};
                    };
                    std::vector<Candidate> candidates;
                    candidates.reserve(dirLen);

                    auto signExtend24b = [](uint32_t v24) -> int32_t {
                        return (v24 & 0x00800000u) ? static_cast<int32_t>(v24 | 0xFF000000u) : static_cast<int32_t>(v24);
                    };

                    for (uint32_t i = 1; i <= static_cast<uint32_t>(dirLen); ++i) {
                        const uint32_t entry = ROMParser::SwapBE32(node4.partialROM.rawQuadlets[absDirOffset + i]);
                        const uint8_t keyType = static_cast<uint8_t>((entry >> 30) & 0x3u);
                        const uint8_t keyId = static_cast<uint8_t>((entry >> 24) & 0x3Fu);
                        const uint32_t value = entry & 0x00FFFFFFu;

                        if (keyId != ASFW::FW::ConfigKey::kTextualDescriptor || keyType != ASFW::FW::EntryType::kLeaf) {
                            continue;
                        }

                        const int32_t off = signExtend24b(value);
                        const int32_t rel = static_cast<int32_t>(i) + off;
                        if (rel < 0) {
                            continue;
                        }

                        const uint32_t absLeaf = absDirOffset + static_cast<uint32_t>(rel);
                        candidates.push_back(Candidate{.absLeaf = absLeaf});
                    }

                    if (candidates.empty()) {
                        if (done) done("");
                        return;
                    }

                    auto idx = std::make_shared<size_t>(0);
                    auto doneShared = std::make_shared<std::function<void(std::string)>>(std::move(done));
                    auto tryNext = std::make_shared<std::function<void()>>();
                    *tryNext = [this, nodeId, candidates = std::move(candidates), idx, tryNext, fetchTextLeafAt, doneShared]() mutable {
                        if (*idx >= candidates.size()) {
                            if (*doneShared) (*doneShared)("");
                            return;
                        }

                        const uint32_t absLeaf = candidates[*idx].absLeaf;
                        (*idx)++;

                        (*fetchTextLeafAt)(absLeaf, [tryNext, doneShared](std::string text) mutable {
                            if (!text.empty()) {
                                if (*doneShared) (*doneShared)(std::move(text));
                                return;
                            }
                            (*tryNext)();
                        });
                    };

                    (*tryNext)();
                });
            });
        };

        auto fetchAssociatedText = [this, nodeId, rootDirStart, fetchTextLeafAt, fetchDescriptorDirText](std::optional<DescriptorRef> ref,
                                                                                                         std::function<void(std::string)> done) {
            if (!ref.has_value()) {
                if (done) done("");
                return;
            }

            const uint32_t abs = rootDirStart + ref->targetRel;
            if (ref->keyType == ASFW::FW::EntryType::kLeaf) {
                (*fetchTextLeafAt)(abs, std::move(done));
                return;
            }
            if (ref->keyType == ASFW::FW::EntryType::kDirectory) {
                (*fetchDescriptorDirText)(abs, std::move(done));
                return;
            }
            if (done) done("");
        };

        // Process vendor/model names first, then unit directories.
        auto processUnitDirs = std::make_shared<std::function<void(size_t)>>();

        auto finalize = [this, nodeId]() {
            auto itF = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                    [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
            if (itF == nodeScans_.end()) {
                CheckAndNotifyCompletion();
                ScheduleAdvanceFSM();
                return;
            }

            auto& nodeF = *itF;
            speedPolicy_.RecordSuccess(nodeId, nodeF.currentSpeed);
            nodeF.state = NodeState::Complete;
            completedROMs_.push_back(std::move(nodeF.partialROM));
            ASFW_LOG_V2(ConfigROM, "FSM: Node %u → Complete ✓ (total complete=%zu)", nodeId, completedROMs_.size());
            CheckAndNotifyCompletion();
            ScheduleAdvanceFSM();
        };

        *processUnitDirs = [this, nodeId, rootDirStart, unitDirRelOffsets = std::move(unitDirRelOffsets),
                            fetchTextLeafAt, fetchDescriptorDirText, signExtend24, processUnitDirs, finalize](size_t idx) mutable {
            if (idx >= unitDirRelOffsets.size()) {
                finalize();
                return;
            }

            const uint32_t unitRel = unitDirRelOffsets[idx];
            const uint32_t absUnitDir = rootDirStart + unitRel;

            // Ensure header present
            this->EnsurePrefix(nodeId, absUnitDir + 1, [this, nodeId, absUnitDir, unitRel, idx, unitDirRelOffsets,
                                                        fetchTextLeafAt, fetchDescriptorDirText, signExtend24, processUnitDirs, finalize](bool ok) mutable {
                if (!ok) {
                    (*processUnitDirs)(idx + 1);
                    return;
                }

                auto itU = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                        [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
                if (itU == nodeScans_.end()) {
                    finalize();
                    return;
                }

                auto& nodeU = *itU;
                if (absUnitDir >= nodeU.partialROM.rawQuadlets.size()) {
                    (*processUnitDirs)(idx + 1);
                    return;
                }

                const uint32_t hdr = ROMParser::SwapBE32(nodeU.partialROM.rawQuadlets[absUnitDir]);
                uint16_t dirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);
                if (dirLen == 0) {
                    (*processUnitDirs)(idx + 1);
                    return;
                }
                if (dirLen > 32) {
                    dirLen = 32;
                }

                const uint32_t dirEndExclusive = absUnitDir + 1u + static_cast<uint32_t>(dirLen);
                this->EnsurePrefix(nodeId, dirEndExclusive, [this, nodeId, absUnitDir, unitRel, dirLen, idx, unitDirRelOffsets,
                                                            fetchTextLeafAt, fetchDescriptorDirText, signExtend24, processUnitDirs, finalize](bool ok2) mutable {
                    if (!ok2) {
                        (*processUnitDirs)(idx + 1);
                        return;
                    }

                    auto itU2 = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                             [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
                    if (itU2 == nodeScans_.end()) {
                        finalize();
                        return;
                    }

                    auto& nodeU2 = *itU2;
                    if (absUnitDir + dirLen >= nodeU2.partialROM.rawQuadlets.size()) {
                        (*processUnitDirs)(idx + 1);
                        return;
                    }

                    std::vector<DirEntry> unitEntries;
                    unitEntries.reserve(dirLen);
                    for (uint32_t i = 1; i <= static_cast<uint32_t>(dirLen); ++i) {
                        const uint32_t entry = ROMParser::SwapBE32(nodeU2.partialROM.rawQuadlets[absUnitDir + i]);
                        DirEntry e{};
                        e.index = i;
                        e.keyType = static_cast<uint8_t>((entry >> 30) & 0x3u);
                        e.keyId = static_cast<uint8_t>((entry >> 24) & 0x3Fu);
                        e.value = entry & 0x00FFFFFFu;

                        if (e.keyType == ASFW::FW::EntryType::kLeaf || e.keyType == ASFW::FW::EntryType::kDirectory) {
                            const int32_t off = (e.value & 0x00800000u) ? static_cast<int32_t>(e.value | 0xFF000000u) : static_cast<int32_t>(e.value);
                            const int32_t rel = static_cast<int32_t>(i) + off;
                            if (rel >= 0) {
                                e.hasTarget = true;
                                e.targetRel = static_cast<uint32_t>(rel);
                            }
                        }
                        unitEntries.push_back(e);
                    }

                    UnitDirectory parsed{};
                    parsed.offsetQuadlets = unitRel;

                    for (const auto& e : unitEntries) {
                        if (e.keyType != ASFW::FW::EntryType::kImmediate) {
                            continue;
                        }
                        switch (e.keyId) {
                            case ASFW::FW::ConfigKey::kUnitSpecId:
                                parsed.unitSpecId = e.value;
                                break;
                            case ASFW::FW::ConfigKey::kUnitSwVersion:
                                parsed.unitSwVersion = e.value;
                                break;
                            case ASFW::FW::ConfigKey::kUnitDependentInfo: // Logical_Unit_Number keyId is 0x14 in TA
                                parsed.logicalUnitNumber = e.value;
                                break;
                            case ASFW::FW::ConfigKey::kModelId:
                                parsed.modelId = e.value;
                                break;
                            default:
                                break;
                        }
                    }

                    // Optional: model name descriptor adjacent to Model_ID in the unit directory.
                    std::optional<DescriptorRef> unitModelDesc;
                    for (size_t i = 0; i < unitEntries.size(); ++i) {
                        const auto& e = unitEntries[i];
                        if (e.keyType != ASFW::FW::EntryType::kImmediate || e.keyId != ASFW::FW::ConfigKey::kModelId) {
                            continue;
                        }
                        if (i + 1 >= unitEntries.size()) {
                            break;
                        }
                        const auto& d = unitEntries[i + 1];
                        if (d.keyId != ASFW::FW::ConfigKey::kTextualDescriptor) {
                            break;
                        }
                        if ((d.keyType == ASFW::FW::EntryType::kLeaf || d.keyType == ASFW::FW::EntryType::kDirectory) &&
                            d.hasTarget && d.targetRel != 0) {
                            unitModelDesc = DescriptorRef{.keyType = d.keyType, .targetRel = d.targetRel};
                        }
                        break;
                    }

                    auto storeAndContinue = [this, nodeId, parsed = std::move(parsed), processUnitDirs, idx](std::optional<std::string> modelNameOpt) mutable {
                        auto itS = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                                [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
                        if (itS != nodeScans_.end()) {
                            auto& nodeS = *itS;
                            UnitDirectory unit = std::move(parsed);
                            if (modelNameOpt.has_value() && !modelNameOpt->empty()) {
                                unit.modelName = *modelNameOpt;
                            }
                            nodeS.partialROM.unitDirectories.push_back(std::move(unit));
                        }
                        (*processUnitDirs)(idx + 1);
                    };

                    if (!unitModelDesc.has_value()) {
                        storeAndContinue(std::nullopt);
                        return;
                    }

                    const uint32_t absDesc = absUnitDir + unitModelDesc->targetRel;
                    if (unitModelDesc->keyType == ASFW::FW::EntryType::kLeaf) {
                        (*fetchTextLeafAt)(absDesc, [storeAndContinue](std::string text) mutable {
                            if (!text.empty()) {
                                storeAndContinue(text);
                            } else {
                                storeAndContinue(std::nullopt);
                            }
                        });
                        return;
                    }
                    if (unitModelDesc->keyType == ASFW::FW::EntryType::kDirectory) {
                        (*fetchDescriptorDirText)(absDesc, [storeAndContinue](std::string text) mutable {
                            if (!text.empty()) {
                                storeAndContinue(text);
                            } else {
                                storeAndContinue(std::nullopt);
                            }
                        });
                        return;
                    }

                    storeAndContinue(std::nullopt);
                });
            });
        };

        fetchAssociatedText(vendorDesc, [this, nodeId, modelDesc, fetchAssociatedText, processUnitDirs](std::string vendor) mutable {
            auto itN = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                    [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
            if (itN != nodeScans_.end() && !vendor.empty()) {
                itN->partialROM.vendorName = vendor;
            }

            fetchAssociatedText(modelDesc, [this, nodeId, processUnitDirs](std::string model) mutable {
                auto itM = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                        [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
                if (itM != nodeScans_.end() && !model.empty()) {
                    itM->partialROM.modelName = model;
                }

                (*processUnitDirs)(0);
            });
        });
    });
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
    // Never call AdvanceFSM() directly from within completion callbacks of async operations.
    // Schedule it onto the discovery dispatch queue to avoid re-entrancy issues.
    DispatchAsync(^{
        this->AdvanceFSM();
    });
}

void ROMScanner::EnsurePrefix(uint8_t nodeId,
                              uint32_t requiredTotalQuadlets,
                              std::function<void(bool)> completion) {
    constexpr uint32_t kMaxROMQuadlets = 256;

    auto it = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                           [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
    if (it == nodeScans_.end()) {
        if (completion) completion(false);
        return;
    }

    auto& node = *it;

    if (requiredTotalQuadlets > kMaxROMQuadlets) {
        ASFW_LOG(ConfigROM,
                 "⚠️  EnsurePrefix: node=%u required=%u exceeds max ROM prefix (%u quadlets), skipping",
                 nodeId, requiredTotalQuadlets, kMaxROMQuadlets);
        if (completion) completion(false);
        return;
    }

    const uint32_t have = static_cast<uint32_t>(node.partialROM.rawQuadlets.size());
    if (have >= requiredTotalQuadlets) {
        if (completion) completion(true);
        return;
    }

    const uint32_t toRead = requiredTotalQuadlets - have;
    const uint32_t offsetBytes = have * 4u;

    ASFW_LOG_V3(ConfigROM, "EnsurePrefix: node=%u have=%u need=%u (read %u quadlets at offsetBytes=%u)",
                nodeId, have, requiredTotalQuadlets, toRead, offsetBytes);

    inflightCount_++;

    auto callback = [this, nodeId, requiredTotalQuadlets, completion = std::move(completion)](const ROMReader::ReadResult& res) mutable {
        inflightCount_--;

        auto it2 = std::find_if(nodeScans_.begin(), nodeScans_.end(),
                                [nodeId](const NodeScanState& n) { return n.nodeId == nodeId; });
        if (it2 == nodeScans_.end()) {
            if (completion) completion(false);
            CheckAndNotifyCompletion();
            ScheduleAdvanceFSM();
            return;
        }

        auto& node2 = *it2;

        if (!res.success || !res.data || res.dataLength == 0) {
            ASFW_LOG(ConfigROM, "⚠️  EnsurePrefix read failed: node=%u", nodeId);
            if (completion) completion(false);
            CheckAndNotifyCompletion();
            ScheduleAdvanceFSM();
            return;
        }

        const uint32_t gotQuadlets = res.dataLength / 4;
        node2.partialROM.rawQuadlets.reserve(node2.partialROM.rawQuadlets.size() + gotQuadlets);
        for (uint32_t i = 0; i < gotQuadlets; ++i) {
            node2.partialROM.rawQuadlets.push_back(res.data[i]);
        }

        const bool ok = node2.partialROM.rawQuadlets.size() >= requiredTotalQuadlets;
        if (!ok) {
            ASFW_LOG_V2(ConfigROM,
                        "⚠️  EnsurePrefix short read: node=%u have=%zu required=%u",
                        nodeId, node2.partialROM.rawQuadlets.size(), requiredTotalQuadlets);
        }

        if (completion) completion(ok);
        CheckAndNotifyCompletion();
        ScheduleAdvanceFSM();
    };

    reader_->ReadRootDirQuadlets(nodeId,
                                 currentGen_,
                                 node.currentSpeed,
                                 offsetBytes,
                                 toRead,
                                 callback);
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

    if (completionNotified_) {
        return;
    }
    completionNotified_ = true;

    const Generation genToReport = currentGen_;

    if (onScanComplete_) {
        ASFW_LOG_V1(ConfigROM, "✅ ROMScanner: Scan complete for gen=%u, notifying immediately", genToReport);
        onScanComplete_(genToReport);
    } else {
        ASFW_LOG(ConfigROM, "⚠️  ROMScanner: Scan complete for gen=%u but NO callback set!", genToReport);
    }

    // Mark scanner idle after the completion callback returns. The callback is expected
    // to synchronously drain results via DrainReady(genToReport).
    currentGen_ = 0;
}

bool ROMScanner::TriggerManualRead(uint8_t nodeId, Generation gen, const Driver::TopologySnapshot& topology) {
    // If scanner is idle (currentGen_ == 0), we need to reinitialize it with the current generation
    // This happens after automatic scan completes and scanner marks itself idle
    if (currentGen_ == 0 && gen != 0) {
        ASFW_LOG_V2(ConfigROM, "TriggerManualRead: scanner idle, restarting with gen=%u for node=%u",
                 gen, nodeId);
        // Set generation and prepare for manual scan
        currentGen_ = gen;
        completionNotified_ = false;
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
