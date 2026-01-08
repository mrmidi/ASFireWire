#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <atomic>
#include <array>

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

// Isochronous Receive metrics for GUI export
// All counters are atomic for safe concurrent access from Poll() path
struct IsochRxMetrics {
    // Packet counters
    std::atomic<uint64_t> totalPackets{0};
    std::atomic<uint64_t> dataPackets{0};   // 80-byte (with samples)
    std::atomic<uint64_t> emptyPackets{0};  // 16-byte (no samples)
    std::atomic<uint64_t> drops{0};         // DBC discontinuities
    std::atomic<uint64_t> errors{0};        // CIP parse errors
    
    // Latency histogram buckets (in µs)
    // [0]: <100µs, [1]: 100-500µs, [2]: 500-1000µs, [3]: >1000µs
    static constexpr size_t kLatencyBuckets = 4;
    std::array<std::atomic<uint64_t>, kLatencyBuckets> latencyHist{};
    
    // Last poll cycle info
    std::atomic<uint32_t> lastPollLatencyUs{0};
    std::atomic<uint32_t> lastPollPackets{0};
    
    // CIP header snapshot
    std::atomic<uint8_t> cipSID{0};
    std::atomic<uint8_t> cipDBS{0};
    std::atomic<uint8_t> cipFDF{0};
    std::atomic<uint16_t> cipSYT{0xFFFF};
    std::atomic<uint8_t> cipDBC{0};
    
    // Helper to record latency
    void RecordLatency(uint32_t microseconds) {
        lastPollLatencyUs.store(microseconds, std::memory_order_relaxed);
        if (microseconds < 100) {
            latencyHist[0].fetch_add(1, std::memory_order_relaxed);
        } else if (microseconds < 500) {
            latencyHist[1].fetch_add(1, std::memory_order_relaxed);
        } else if (microseconds < 1000) {
            latencyHist[2].fetch_add(1, std::memory_order_relaxed);
        } else {
            latencyHist[3].fetch_add(1, std::memory_order_relaxed);
        }
    }
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
    
    // Isoch RX metrics (atomic, safe from Poll path)
    IsochRxMetrics& IsochRx() { return isochRx_; }
    const IsochRxMetrics& IsochRx() const { return isochRx_; }

private:
    std::unordered_map<std::string, uint64_t> counters_;
    BusResetMetrics busReset_{};
    TopologyMetrics topology_{};
    IsochRxMetrics isochRx_{};
};

} // namespace ASFW::Driver


