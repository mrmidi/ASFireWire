#pragma once

#include <optional>

#include "ControllerTypes.hpp"
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

private:
    std::optional<TopologySnapshot> latest_;
};

} // namespace ASFW::Driver
