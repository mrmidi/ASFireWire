#include "TopologyManager.hpp"

#include <algorithm>
#include <numeric>

#include "../Logging/Logging.hpp"
#include "SelfIDStreamParser.hpp"
#include "SelfIDTopologyNormalizer.hpp"

namespace {

using namespace ASFW::Driver;

struct NodeIDRegisterInfo {
    std::optional<uint8_t> localNodeId;
    std::optional<uint16_t> busNumber;
    uint16_t busBase16{0};
};

[[nodiscard]] NodeIDRegisterInfo DecodeNodeIDRegister(uint32_t nodeIDReg) {
    NodeIDRegisterInfo info{};
    if ((nodeIDReg & 0x80000000u) == 0) {
        return info;
    }

    const uint16_t nodeID = static_cast<uint16_t>(nodeIDReg & 0xFFFFu);
    const uint8_t nodeNum = static_cast<uint8_t>(nodeID & 0x3Fu);
    info.busBase16 = static_cast<uint16_t>(nodeID & 0xFFC0u);
    info.busNumber = static_cast<uint16_t>((nodeID >> 6) & 0x3FFu);
    if (nodeNum != 63) {
        info.localNodeId = nodeNum;
    }
    return info;
}

uint8_t CalculateOptimumGapCount(const std::vector<SelfIDNodeRecord>& records) {
    uint8_t maxGap = 0;
    for (const auto& record : records) {
        if (record.gapCount > maxGap) {
            maxGap = record.gapCount;
        }
    }
    return std::min<uint8_t>(maxGap, 63);
}

bool CalculateGapConsistency(const std::vector<uint32_t>& quads) {
    const auto gaps = TopologyManager::ExtractGapCounts(quads);
    if (gaps.empty()) return true;
    
    const uint8_t first = gaps[0];
    return std::all_of(gaps.begin(), gaps.end(), [first](uint8_t g) { return g == first; });
}

void LogTopologySummary(const TopologySnapshot& snapshot) {
    const std::string rootStr = (snapshot.rootNodeId != kInvalidPhysicalId) 
                                ? std::to_string(snapshot.rootNodeId) : "none";
    const std::string irmStr = (snapshot.irmNodeId != kInvalidPhysicalId) 
                                ? std::to_string(snapshot.irmNodeId) : "none";
    const std::string localStr = (snapshot.localNodeId != kInvalidPhysicalId) 
                                 ? std::to_string(snapshot.localNodeId) : "none";
    const std::string busStr = snapshot.busNumber.has_value() 
                                ? std::to_string(*snapshot.busNumber) : "none";

    ASFW_LOG(Topology, "=== 🗺️ Topology Snapshot v2 ===");
    ASFW_LOG(Topology,
             "🧮 gen=%u nodes=%u root=%{public}s IRM=%{public}s local=%{public}s bus=%{public}s gap=%u hops=%u status=%u error=%u",
             snapshot.generation,
             snapshot.nodeCount,
             rootStr.c_str(),
             irmStr.c_str(),
             localStr.c_str(),
             busStr.c_str(),
             snapshot.gapCount,
             snapshot.physical.maxHopsFromRoot,
             static_cast<uint8_t>(snapshot.graphStatus),
             static_cast<uint8_t>(snapshot.errorCode));
}

} // namespace

namespace ASFW::Driver {

TopologyManager::TopologyManager() {
    badIRMFlags_.assign(63, false);
}

void TopologyManager::Reset() {
    latest_.reset();
}

void TopologyManager::InvalidateForBusReset() {
    latest_.reset();
    ClearBadIRMFlags();
}

const char* TopologyManager::TopologyBuildErrorCodeString(TopologyBuildErrorCode code) noexcept {
    switch (code) {
    case TopologyBuildErrorCode::None: return "None";
    case TopologyBuildErrorCode::InvalidSelfID: return "InvalidSelfID";
    case TopologyBuildErrorCode::EmptySequenceSet: return "EmptySequenceSet";
    case TopologyBuildErrorCode::NonContiguousPhysicalIds: return "NonContiguousPhysicalIds";
    case TopologyBuildErrorCode::DuplicatePhysicalId: return "DuplicatePhysicalId";
    case TopologyBuildErrorCode::MissingBasePacket: return "MissingBasePacket";
    case TopologyBuildErrorCode::InvalidExtendedPacketOrder: return "InvalidExtendedPacketOrder";
    case TopologyBuildErrorCode::NonRootWithoutParentPort: return "NonRootWithoutParentPort";
    case TopologyBuildErrorCode::RootHasParentPort: return "RootHasParentPort";
    case TopologyBuildErrorCode::ChildPortWithEmptyStack: return "ChildPortWithEmptyStack";
    case TopologyBuildErrorCode::PoppedNodeHasNoUnresolvedParent: return "PoppedNodeHasNoUnresolvedParent";
    case TopologyBuildErrorCode::UnresolvedStackAfterRoot: return "UnresolvedStackAfterRoot";
    case TopologyBuildErrorCode::ReciprocalLinkMissing: return "ReciprocalLinkMissing";
    case TopologyBuildErrorCode::EdgeCountMismatch: return "EdgeCountMismatch";
    case TopologyBuildErrorCode::LocalNodeUnavailable: return "LocalNodeUnavailable";
    }
    return "Unknown";
}

std::expected<TopologySnapshot, TopologyBuildError>
TopologyManager::UpdateFromSelfID(const SelfIDCapture::Result& result,
                                  uint64_t timestamp,
                                  uint32_t nodeIDReg) {
    TopologySnapshot snapshot{};
    snapshot.generation = result.generation;
    snapshot.capturedAt = timestamp;
    snapshot.rawSelfIdQuadlets = result.quads;

    if (!result.valid || result.quads.empty()) {
        snapshot.selfIdStatus = result.timedOut ? SelfIDStreamStatus::Timeout
                                                : SelfIDStreamStatus::Invalid;
        snapshot.graphStatus = TopologyGraphStatus::Invalid;
        snapshot.errorCode = TopologyBuildErrorCode::InvalidSelfID;
        snapshot.errorDetail = "Self-ID stream is invalid or empty";
        return std::unexpected(TopologyBuildError{snapshot.errorCode, snapshot.errorDetail});
    }

    snapshot.selfIdStatus = SelfIDStreamStatus::Valid;

    const NodeIDRegisterInfo nodeInfo = DecodeNodeIDRegister(nodeIDReg);
    snapshot.localNodeId = nodeInfo.localNodeId.value_or(kInvalidPhysicalId);
    snapshot.busBase16 = nodeInfo.busBase16;
    snapshot.busNumber = nodeInfo.busNumber;

    auto records = SelfIDStreamParser::Parse(result);
    if (!records.has_value()) {
        snapshot.graphStatus = TopologyGraphStatus::Invalid;
        snapshot.errorCode = records.error().code;
        snapshot.errorDetail = records.error().detail;
        return std::unexpected(records.error());
    }

    auto physical = SelfIDTopologyNormalizer::BuildPhysicalGraph(*records, snapshot.localNodeId);
    if (!physical.has_value()) {
        snapshot.graphStatus = TopologyGraphStatus::Invalid;
        snapshot.errorCode = physical.error().code;
        snapshot.errorDetail = physical.error().detail;
        return std::unexpected(physical.error());
    }

    auto normalized =
        SelfIDTopologyNormalizer::NormalizeFromLocal(*physical, snapshot.localNodeId);
    if (!normalized.has_value()) {
        // Normalization failure is treated as a graph failure in this implementation.
        snapshot.graphStatus = TopologyGraphStatus::Invalid;
        snapshot.errorCode = normalized.error().code;
        snapshot.errorDetail = normalized.error().detail;
        return std::unexpected(normalized.error());
    }

    snapshot.graphStatus = TopologyGraphStatus::Valid;
    snapshot.physical = *physical;
    snapshot.normalizedFromLocal = *normalized;

    snapshot.nodeCount = snapshot.physical.nodeCount;
    snapshot.rootNodeId = snapshot.physical.rootId;
    snapshot.irmNodeId = snapshot.physical.irmId;
    snapshot.gapCount = CalculateOptimumGapCount(*records);
    snapshot.gapCountConsistent = CalculateGapConsistency(result.quads);

    LogTopologySummary(snapshot);

    latest_ = snapshot;
    return snapshot;
}

std::optional<TopologySnapshot> TopologyManager::LatestSnapshot() const {
    return latest_;
}

std::optional<TopologySnapshot>
TopologyManager::CompareAndSwap(std::optional<TopologySnapshot> previous) {
    if (!latest_.has_value()) {
        return std::nullopt;
    }
    if (previous.has_value() && previous->capturedAt == latest_->capturedAt) {
        return std::nullopt;
    }
    return latest_;
}

void TopologyManager::MarkNodeAsBadIRM(uint8_t nodeID) {
    if (nodeID >= 63) return;
    if (badIRMFlags_.size() < 63) {
        badIRMFlags_.resize(63, false);
    }
    if (!badIRMFlags_[nodeID]) {
        ASFW_LOG(Topology, "⚠️  Node %u marked as bad IRM (failed verification)", nodeID);
        badIRMFlags_[nodeID] = true;
    }
}

bool TopologyManager::IsNodeBadIRM(uint8_t nodeID) const {
    if (nodeID >= badIRMFlags_.size()) return false;
    return badIRMFlags_[nodeID];
}

void TopologyManager::ClearBadIRMFlags() {
    badIRMFlags_.assign(63, false);
}

std::vector<uint8_t> TopologyManager::ExtractGapCounts(const std::vector<uint32_t>& selfIDs) {
    std::vector<uint8_t> gaps;
    for (uint32_t packet : selfIDs) {
        if (!IsSelfIDTag(packet) || IsExtended(packet)) continue;
        gaps.push_back(ExtractGapCount(packet));
    }
    return gaps;
}

} // namespace ASFW::Driver
