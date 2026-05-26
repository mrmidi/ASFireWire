#include "SpeedPolicy.hpp"
#include "DiscoveryValues.hpp"  // For MaxPayload constants
#include "../Logging/Logging.hpp"

namespace ASFW::Discovery {

SpeedPolicy::SpeedPolicy() = default;

namespace {
uint32_t SpeedMbps(FwSpeed speed) {
    return 100u << static_cast<uint8_t>(speed);
}
} // namespace

LinkPolicy SpeedPolicy::ForNode(uint8_t nodeId) const {
    LinkPolicy policy{};
    
    auto it = nodeStates_.find(nodeId);
    if (it != nodeStates_.end()) {
        policy.localToNode = it->second.currentSpeed;
    } else {
        policy.localToNode = FwSpeed::S400;
    }
    
    policy.maxPayloadBytes = ComputeMaxPayload(policy.localToNode);
    policy.halvePackets = halfSizePackets_;
    
    return policy;
}

void SpeedPolicy::RecordSuccess(uint8_t nodeId, FwSpeed speed) {
    auto& state = nodeStates_[nodeId];
    state.currentSpeed = speed;
    state.successCount++;
    // Reset timeout counter on success
    state.timeoutCount = 0;
    
    // Rate-limited success logging
    ASFW_LOG_RL(Discovery, "speed_success", 5000, OS_LOG_TYPE_DEBUG,
                "Node %u: Success at S%u (total=%u)",
                nodeId, SpeedMbps(speed), state.successCount);
}

void SpeedPolicy::RecordTimeout(uint8_t nodeId, FwSpeed speed) {
    auto& state = nodeStates_[nodeId];
    state.currentSpeed = speed;
    state.timeoutCount++;
    
    ASFW_LOG(Discovery, "Node %u: Timeout at S%u (count=%u)",
             nodeId, SpeedMbps(speed), state.timeoutCount);
    
    // ROMScanSession calls this only after the per-step retry budget is exhausted.
    // Downgrade one tier immediately so discovery really follows S400→S200→S100.
    FwSpeed downgraded = DowngradeSpeed(speed);
    if (downgraded != speed) {
        state.currentSpeed = downgraded;
        state.timeoutCount = 0;
        ASFW_LOG(Discovery, "Node %u: Downgraded S%u → S%u",
                 nodeId, SpeedMbps(speed), SpeedMbps(downgraded));
    }
}

void SpeedPolicy::SetHalfSizePackets(bool enabled) {
    halfSizePackets_ = enabled;
}

void SpeedPolicy::Reset() {
    nodeStates_.clear();
}

uint16_t SpeedPolicy::ComputeMaxPayload(FwSpeed speed) const {
    uint16_t basePayload = 0;
    
    switch (speed) {
        case FwSpeed::S100: basePayload = MaxPayload::kS100; break;
        case FwSpeed::S200: basePayload = MaxPayload::kS200; break;
        case FwSpeed::S400: basePayload = MaxPayload::kS400; break;
        case FwSpeed::S800: basePayload = MaxPayload::kS800; break;
    }
    
    if (halfSizePackets_) {
        basePayload /= 2;
    }
    
    return basePayload;
}

FwSpeed SpeedPolicy::DowngradeSpeed(FwSpeed current) const {
    switch (current) {
        case FwSpeed::S800: return FwSpeed::S400;
        case FwSpeed::S400: return FwSpeed::S200;
        case FwSpeed::S200: return FwSpeed::S100;
        case FwSpeed::S100: return FwSpeed::S100;  // Can't go lower
    }
    return FwSpeed::S100;
}

} // namespace ASFW::Discovery
