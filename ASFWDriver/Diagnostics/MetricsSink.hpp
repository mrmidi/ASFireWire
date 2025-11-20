#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "../Controller/ControllerTypes.hpp"

namespace ASFW::Driver {

// Aggregated topology/Self-ID metrics for GUI export
struct TopologyMetrics {
    uint64_t lastSuccessfulDecode{0};     // Timestamp of last valid decode
    uint32_t totalDecodes{0};             // Total Self-ID decode attempts
    uint32_t successfulDecodes{0};        // Successful decodes
    uint32_t crcErrors{0};                // CRC validation failures
    uint32_t timeouts{0};                 // Self-ID timeout failures
    uint32_t validationErrors{0};         // Sequence/structure validation errors
    uint32_t maxNodesObserved{0};         // Max node count ever seen
    std::optional<SelfIDMetrics> latestSelfID;  // Most recent Self-ID capture
};

// Central aggregation point for lightweight counters and structured log data.
class MetricsSink {
public:
    MetricsSink();

    void Increment(std::string_view key);
    void SetGauge(std::string_view key, uint64_t value);

    const std::unordered_map<std::string, uint64_t>& Counters() const { return counters_; }
    const BusResetMetrics& BusReset() const { return busReset_; }
    BusResetMetrics& BusReset() { return busReset_; }
    
    const TopologyMetrics& Topology() const { return topology_; }
    TopologyMetrics& Topology() { return topology_; }

private:
    std::unordered_map<std::string, uint64_t> counters_;
    BusResetMetrics busReset_{};
    TopologyMetrics topology_{};
};

} // namespace ASFW::Driver

