#pragma once

#include <cstdint>
#include <unordered_map>

#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Central authority for link speed and max payload policy.
// Provides speed fallback sequencing (S400→S200→S100) and per-node adaptation
// based on observed transaction outcomes.
class SpeedPolicy {
public:
    SpeedPolicy();
    ~SpeedPolicy() = default;

    // Query current policy for a node
    LinkPolicy ForNode(uint8_t nodeId) const;

    // Adapt policy based on transaction outcomes
    void RecordSuccess(uint8_t nodeId, FwSpeed speed);
    void RecordTimeout(uint8_t nodeId, FwSpeed speed);

    // Admin override: halve packet sizes globally (escape hatch for flaky topologies)
    void SetHalfSizePackets(bool enabled);

    // Reset all per-node state (e.g., after bus reset)
    void Reset();

private:
    struct NodeSpeedState {
        FwSpeed currentSpeed{FwSpeed::S100};
        uint8_t timeoutCount{0};
        uint8_t successCount{0};
    };

    // Compute max payload based on speed and policy flags
    uint16_t ComputeMaxPayload(FwSpeed speed) const;

    // Downgrade speed to next lower tier
    FwSpeed DowngradeSpeed(FwSpeed current) const;

    std::unordered_map<uint8_t, NodeSpeedState> nodeStates_;
    bool halfSizePackets_{false};
};

} // namespace ASFW::Discovery

