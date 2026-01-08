#pragma once

#include <optional>
#include <vector>

#include "../Controller/ControllerTypes.hpp"
#include "SelfIDCapture.hpp"

namespace ASFW::Driver {

// Transforms decoded Self-ID data into immutable topology snapshots and offers
// diffing support so the service can log concise bus changes.
class TopologyManager {
public:
    TopologyManager();

    void Reset();
    std::optional<TopologySnapshot> UpdateFromSelfID(const SelfIDCapture::Result& result,
                                                     uint64_t timestamp,
                                                     uint32_t nodeIDReg);

    std::optional<TopologySnapshot> LatestSnapshot() const;
    std::optional<TopologySnapshot> CompareAndSwap(std::optional<TopologySnapshot> previous);

    void MarkNodeAsBadIRM(uint8_t nodeID);

    bool IsNodeBadIRM(uint8_t nodeID) const;

    const std::vector<bool>& GetBadIRMFlags() const { return badIRMFlags_; }

    void ClearBadIRMFlags();

    static std::vector<uint8_t> ExtractGapCounts(const std::vector<uint32_t>& selfIDs);

private:
    std::optional<TopologySnapshot> latest_;

    /// Per-node bad IRM flags (indexed by node ID, 0-62)
    /// true = node failed IRM verification (read/CAS test)
    std::vector<bool> badIRMFlags_;
};

} // namespace ASFW::Driver
