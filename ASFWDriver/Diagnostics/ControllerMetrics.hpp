#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Debug {
class BusResetPacketCapture;
}

namespace ASFW::Driver {

/// Centralized metrics collection for the FireWire controller
///
/// Tracks runtime statistics for monitoring and debugging.
/// Future: Will integrate with IOReporter framework for system-level visibility
class ControllerMetrics {
public:
    ControllerMetrics();
    ~ControllerMetrics();

    // Bus Reset Metrics
    void RecordBusReset(uint32_t generation);
    uint64_t GetBusResetCount() const;
    uint32_t GetCurrentGeneration() const;
    uint64_t GetLastResetTimestamp() const;

    // Packet Counters
    void RecordARRequestPacket(size_t bytes);
    void RecordARResponsePacket(size_t bytes);
    void RecordATRequestCompleted();
    void RecordATResponseCompleted();

    uint64_t GetARRequestPacketCount() const;
    uint64_t GetARResponsePacketCount() const;
    uint64_t GetATRequestCompletedCount() const;
    uint64_t GetATResponseCompletedCount() const;

    // Node Topology
    void SetNodeCount(uint8_t count);
    void SetLocalNodeID(uint8_t nodeID);
    void SetRootNodeID(uint8_t nodeID);
    void SetIRMNodeID(uint8_t nodeID);

    uint8_t GetNodeCount() const;
    uint8_t GetLocalNodeID() const;
    uint8_t GetRootNodeID() const;
    uint8_t GetIRMNodeID() const;

    // State
    void SetControllerState(const char* stateName);
    const char* GetControllerState() const;

    // Uptime
    uint64_t GetUptimeNanoseconds() const;

    // Reset all metrics
    void Reset();

private:
    // Bus reset tracking
    std::atomic<uint64_t> busResetCount_{0};
    std::atomic<uint32_t> currentGeneration_{0};
    std::atomic<uint64_t> lastResetTimestamp_{0};

    // Packet counts
    std::atomic<uint64_t> arRequestPackets_{0};
    std::atomic<uint64_t> arResponsePackets_{0};
    std::atomic<uint64_t> atRequestsCompleted_{0};
    std::atomic<uint64_t> atResponsesCompleted_{0};

    // Topology
    std::atomic<uint8_t> nodeCount_{0};
    std::atomic<uint8_t> localNodeID_{0xFF};
    std::atomic<uint8_t> rootNodeID_{0xFF};
    std::atomic<uint8_t> irmNodeID_{0xFF};

    // State
    char stateName_[32]{"Initializing"};

    // Start time
    uint64_t startTime_{0};
};

} // namespace ASFW::Driver
