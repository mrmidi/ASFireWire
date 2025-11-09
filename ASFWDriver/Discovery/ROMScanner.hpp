#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <functional>

#include "../Core/ControllerTypes.hpp"
#include "DiscoveryTypes.hpp"
#include "ROMReader.hpp"
#include "SpeedPolicy.hpp"

namespace ASFW::Async {
class AsyncSubsystem;
}

namespace ASFW::Discovery {

// Completion callback: called when scan becomes idle (all nodes processed)
using ScanCompletionCallback = std::function<void(Generation)>;

// FSM-driven ROM scanner with bounded concurrency and retry logic.
// Orchestrates per-node BIB and root directory reads with speed fallback.
class ROMScanner {
public:
    explicit ROMScanner(Async::AsyncSubsystem& asyncSubsystem,
                       SpeedPolicy& speedPolicy,
                       ScanCompletionCallback onScanComplete = nullptr);
    ~ROMScanner();

    // Begin scanning nodes from topology for given generation
    // Only scans remote nodes (excludes localNodeId)
    void Begin(Generation gen,
               const Driver::TopologySnapshot& topology,
               uint8_t localNodeId);

    // Check if scan is idle for given generation (all nodes processed)
    bool IsIdleFor(Generation gen) const;

    // Pull completed ROMs for given generation (moves ownership to caller)
    std::vector<ConfigROM> DrainReady(Generation gen);

    // Cancel scan for given generation (abort in-flight operations)
    void Abort(Generation gen);

    // Manually trigger ROM read for a specific node (for GUI debugging)
    // Returns true if read was initiated, false if already in progress or invalid
    // topology: Current topology snapshot (needed for busBase16 if scanner is idle)
    bool TriggerManualRead(uint8_t nodeId, Generation gen, const Driver::TopologySnapshot& topology);

    // Set completion callback (called when scan becomes idle)
    // Can be set after construction to support dependency injection
    void SetCompletionCallback(ScanCompletionCallback callback);

private:
    enum class NodeState : uint8_t {
        Idle,
        ReadingBIB,
        ReadingRootDir,
        Complete,
        Failed
    };

    struct NodeScanState {
        uint8_t nodeId{0xFF};
        NodeState state{NodeState::Idle};
        // TODO: S100 hardcoded for maximum hardware compatibility.
        FwSpeed currentSpeed{FwSpeed::S100};
        uint8_t retriesLeft{0};
        ConfigROM partialROM{};
    };

    // Advance FSM: kick off next read if capacity available
    void AdvanceFSM();

    // Handle BIB read completion
    void OnBIBComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    // Handle root directory read completion
    void OnRootDirComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    // Retry with speed downgrade
    void RetryWithFallback(NodeScanState& node);

    // Check if we have capacity for more in-flight operations
    bool HasCapacity() const;

    // Check if scan is complete and notify callback if so (Apple-style immediate completion)
    void CheckAndNotifyCompletion();

    Async::AsyncSubsystem& async_;
    SpeedPolicy& speedPolicy_;
    ROMScannerParams params_;
    std::unique_ptr<ROMReader> reader_;

    Generation currentGen_{0};
    Driver::TopologySnapshot currentTopology_;  // Store snapshot for bus info access
    std::vector<NodeScanState> nodeScans_;
    std::vector<ConfigROM> completedROMs_;
    uint8_t inflightCount_{0};

    // Completion callback (called when scan becomes idle)
    ScanCompletionCallback onScanComplete_;
};

} // namespace ASFW::Discovery

