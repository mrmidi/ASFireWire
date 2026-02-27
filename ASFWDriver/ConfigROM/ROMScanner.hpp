#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <functional>
#include <optional>
#include <string>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "../Controller/ControllerTypes.hpp"
#include "../Discovery/DiscoveryTypes.hpp"
#include "ROMScanNodeStateMachine.hpp"
#include "ROMScannerEventBus.hpp"
#include "ROMScannerCompletionManager.hpp"
#include "ROMScannerInflightCoordinator.hpp"
#include "ROMScannerFSMController.hpp"
#include "ROMScannerEnsurePrefixController.hpp"
#include "ConfigROMConstants.hpp"
#include "ROMReader.hpp"
#include "SpeedPolicy.hpp"

namespace ASFW::Async {
class IFireWireBus;
}

namespace ASFW::Driver {
class TopologyManager;
}

namespace ASFW::Discovery {

class ROMScannerDiscoveryFlow;
class ROMScannerFSMFlow;
class ROMScannerBIBPhase;
class ROMScannerIRMPhase;

// Completion callback: called when scan becomes idle (all nodes processed)
using ScanCompletionCallback = std::function<void(Generation)>;

// FSM-driven ROM scanner with bounded concurrency and retry logic.
// Orchestrates per-node BIB and root directory reads with speed fallback.
// Also performs IRM capability verification (Phase 3).
class ROMScanner {
public:
    explicit ROMScanner(Async::IFireWireBus& bus,
                        SpeedPolicy& speedPolicy,
                        const ROMScannerParams& params,
                        const ScanCompletionCallback& onScanComplete = nullptr,
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

    void SetCompletionCallback(const ScanCompletionCallback& callback);

    void SetTopologyManager(Driver::TopologyManager* topologyManager);

    /// Returns true if the most recent scan encountered ack_busy_X or
    /// BIB-not-ready (quadlet[0]==0) from any node.  Used by
    /// BusResetCoordinator to decide whether to delay the next discovery.
    [[nodiscard]] bool HadBusyNodes() const { return hadBusyNodes_; }

private:
    friend class ROMScannerDiscoveryFlow;
    friend class ROMScannerFSMFlow;
    friend class ROMScannerBIBPhase;
    friend class ROMScannerIRMPhase;

    using NodeState = ROMScanNodeStateMachine::State;

    void AdvanceFSM();

    void OnBIBComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    void OnIRMReadComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    void OnIRMLockComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    void HandleIRMLockResult(ROMScanNodeStateMachine& node, const ROMReader::ReadResult& result);

    void OnRootDirComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    struct DirEntry {
        uint32_t index{0};
        uint8_t keyType{0};
        uint8_t keyId{0};
        uint32_t value{0};
        bool hasTarget{false};
        uint32_t targetRel{0};
    };

    struct DescriptorRef {
        uint8_t keyType{0};
        uint32_t targetRel{0};
    };

    struct UnitDirStepContext {
        uint8_t nodeId{0xFF};
        uint32_t rootDirStart{0};
        std::vector<uint32_t> unitDirRelOffsets;
        size_t index{0};
        uint32_t absUnitDir{0};
        uint32_t unitRel{0};
        uint16_t dirLen{0};
    };

    void RetryWithFallback(ROMScanNodeStateMachine& node);

    ROMScanNodeStateMachine* FindNodeScan(uint8_t nodeId);

    bool HasCapacity() const;

    bool TransitionNodeState(ROMScanNodeStateMachine& node,
                             NodeState next,
                             const char* reason) const;

    void CheckAndNotifyCompletion();

    void IncrementInflight();
    void DecrementInflight();
    void ResetInflight();
    [[nodiscard]] uint16_t InflightCount() const;

    void ResetCompletionNotification();
    void MarkCompletionNotified();
    [[nodiscard]] bool TryMarkCompletionNotified();

    void PublishReadEvent(ROMScannerEventType type,
                          uint8_t nodeId,
                          const ROMReader::ReadResult& result);
    void PublishEnsurePrefixEvent(uint8_t nodeId,
                                  uint32_t requiredTotalQuadlets,
                                  const std::function<void(bool)>& completion,
                                  const ROMReader::ReadResult& result);
    [[nodiscard]] bool IsCurrentGenerationEvent(const ROMScannerReadEventData& payload) const;
    void ScheduleEventDrain();
    void ProcessPendingEvents();

    Async::IFireWireBus& bus_;
    SpeedPolicy& speedPolicy_;
    ROMScannerParams params_;
    std::unique_ptr<ROMReader> reader_;
    OSSharedPtr<IODispatchQueue> dispatchQueue_;

    Generation currentGen_{0};
    Driver::TopologySnapshot currentTopology_;
    std::vector<ROMScanNodeStateMachine> nodeScans_;
    std::vector<ConfigROM> completedROMs_;
    ROMScannerInflightCoordinator inflight_{};

    ScanCompletionCallback onScanComplete_;
    ROMScannerCompletionManager completionMgr_{};
    bool hadBusyNodes_{false};  // Set when any node returns ack_busy_X or BIB quadlet[0]=0

    Driver::TopologyManager* topologyManager_{nullptr};
    ROMScannerEventBus eventBus_{};
    [[no_unique_address]] ROMScannerFSMController fsmController_{};
    [[no_unique_address]] ROMScannerEnsurePrefixController ensurePrefixController_{};

    void ScheduleAdvanceFSM();
    void DispatchAsync(void (^work)());

    void EnsurePrefix(uint8_t nodeId, uint32_t requiredTotalQuadlets, std::function<void(bool)> completion);
};

} // namespace ASFW::Discovery
