#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <functional>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "../Controller/ControllerTypes.hpp"
#include "../Discovery/DiscoveryTypes.hpp"
#include "ROMReader.hpp"
#include "SpeedPolicy.hpp"

namespace ASFW::Async {
class IFireWireBus;
}

namespace ASFW::Driver {
class TopologyManager;
}

namespace ASFW::Discovery {

// Completion callback: called when scan becomes idle (all nodes processed)
using ScanCompletionCallback = std::function<void(Generation)>;

// FSM-driven ROM scanner with bounded concurrency and retry logic.
// Orchestrates per-node BIB and root directory reads with speed fallback.
// Also performs IRM capability verification (Phase 3).
class ROMScanner {
public:
    explicit ROMScanner(Async::IFireWireBus& bus,
                        SpeedPolicy& speedPolicy,
                        ScanCompletionCallback onScanComplete = nullptr,
                        OSSharedPtr<IODispatchQueue> dispatchQueue = nullptr);
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

    /**
     * Set TopologyManager for bad IRM reporting.
     *
     * Called during initialization to provide callback for marking bad IRMs.
     * When IRM verification fails (read/CAS test), scanner marks node as bad.
     *
     * @param topologyManager Pointer to TopologyManager (optional, may be nullptr)
     *
     * Reference: Apple IOFireWireController.cpp:2697 - sets scan->fIRMisBad
     */
    void SetTopologyManager(Driver::TopologyManager* topologyManager);

private:
    enum class NodeState : uint8_t {
        Idle,
        ReadingBIB,
        VerifyingIRM_Read,   // Phase 1: Read CHANNELS_AVAILABLE register
        VerifyingIRM_Lock,   // Phase 2: CAS test on CHANNELS_AVAILABLE
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

        // IRM verification state (Phase 3)
        bool needsIRMCheck{false};         // True if node is IRM candidate with speed > S100
        bool irmCheckReadDone{false};      // True after read test completes
        bool irmCheckLockDone{false};      // True after CAS test completes
        bool irmIsBad{false};              // True if verification failed
        uint32_t irmBitBucket{0xFFFFFFFF}; // Stores read value from CHANNELS_AVAILABLE

        bool bibInProgress{false};         // Tracks whether a BIB read is active
    };

    // Advance FSM: kick off next read if capacity available
    void AdvanceFSM();

    // Handle BIB read completion
    void OnBIBComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    // Handle IRM read test completion (Phase 3)
    void OnIRMReadComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    // Handle IRM lock test completion (Phase 3)
    void OnIRMLockComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    void HandleIRMLockResult(NodeScanState& node, const ROMReader::ReadResult& result);

    // Handle root directory read completion
    void OnRootDirComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    // Retry with speed downgrade
    void RetryWithFallback(NodeScanState& node);

    // Check if we have capacity for more in-flight operations
    bool HasCapacity() const;

    // Check if scan is complete and notify callback if so (Apple-style immediate completion)
    void CheckAndNotifyCompletion();

    Async::IFireWireBus& bus_;
    SpeedPolicy& speedPolicy_;
    ROMScannerParams params_;
    std::unique_ptr<ROMReader> reader_;

    Generation currentGen_{0};
    Driver::TopologySnapshot currentTopology_;  // Store snapshot for bus info access
    std::vector<NodeScanState> nodeScans_;
    std::vector<ConfigROM> completedROMs_;
    uint16_t inflightCount_{0};

    // Completion callback (called when scan becomes idle)
    ScanCompletionCallback onScanComplete_;

    // TopologyManager callback for marking bad IRMs (Phase 3)
    Driver::TopologyManager* topologyManager_{nullptr};
};

} // namespace ASFW::Discovery
