#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include <DriverKit/IOLib.h>

#include "DiscoveryTypes.hpp"
#include "../Async/Interfaces/ILinkSpeedSource.hpp"

namespace ASFW::Discovery {

// Central authority for link speed and max payload policy.
// Provides speed fallback sequencing (S400→S200→S100) and per-node adaptation
// based on observed transaction outcomes.
//
// Implements Async::ILinkSpeedSource so that what discovery learns here reaches
// every other transaction on the bus. Before that seam existed this knowledge
// was private to the Config-ROM scan: DICE, SBP-2 and AV/C kept transmitting at
// the Self-ID advertised speed and re-hit a link this class had already proven
// unusable.
class SpeedPolicy final : public Async::ILinkSpeedSource {
public:
    SpeedPolicy();
    ~SpeedPolicy() override;

    SpeedPolicy(const SpeedPolicy&) = delete;
    SpeedPolicy& operator=(const SpeedPolicy&) = delete;

    // Query current policy for a node
    LinkPolicy ForNode(uint8_t nodeId) const;

    // Async::ILinkSpeedSource. Reports a speed only for nodes that have actually
    // been exercised; an unseen node yields nullopt so the caller keeps its
    // Self-ID value rather than inheriting ForNode()'s S400 seed as if it were
    // evidence. The seed is a starting point for probing, not an observation.
    [[nodiscard]] std::optional<FW::FwSpeed>
    ObservedSpeed(FW::NodeId nodeId) const noexcept override;

    // Adapt policy based on transaction outcomes
    void RecordSuccess(uint8_t nodeId, FwSpeed speed);
    void RecordTimeout(uint8_t nodeId, FwSpeed speed);

    // Admin override: halve packet sizes globally (escape hatch for flaky topologies)
    void SetHalfSizePackets(bool enabled);

    // Reset all per-node state. MUST be called on every bus reset: node IDs are
    // reassigned by the reset, so an observation keyed to the old numbering can
    // land on a different device in the next generation.
    void Reset();

private:
    struct NodeSpeedState {
        FwSpeed currentSpeed{FwSpeed::S400};
        uint8_t timeoutCount{0};
        uint8_t successCount{0};
    };

    // Compute max payload based on speed and policy flags
    uint16_t ComputeMaxPayload(FwSpeed speed) const;

    // Downgrade speed to next lower tier
    FwSpeed DowngradeSpeed(FwSpeed current) const;

    // ObservedSpeed() is reachable from every FireWireBusImpl::GetSpeed()
    // caller, which since the single-authority change includes protocol code
    // that does not run on the discovery queue (DiceAudioBackend owns its own
    // "com.asfw.audio.dice" queue). RecordTimeout/RecordSuccess insert, and an
    // insert can rehash the map underneath a concurrent find(). Serialize.
    mutable IOLock* lock_{nullptr};
    std::unordered_map<uint8_t, NodeSpeedState> nodeStates_;
    bool halfSizePackets_{false};
};

} // namespace ASFW::Discovery
