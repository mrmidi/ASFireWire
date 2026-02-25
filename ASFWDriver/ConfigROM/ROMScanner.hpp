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
                        const ROMScannerParams& params,
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

    void SetCompletionCallback(ScanCompletionCallback callback);

    void SetTopologyManager(Driver::TopologyManager* topologyManager);

    /// Returns true if the most recent scan encountered ack_busy_X or
    /// BIB-not-ready (quadlet[0]==0) from any node.  Used by
    /// BusResetCoordinator to decide whether to delay the next discovery.
    [[nodiscard]] bool HadBusyNodes() const { return hadBusyNodes_; }

private:
    enum class NodeState : uint8_t {
        Idle,
        ReadingBIB,
        VerifyingIRM_Read,
        VerifyingIRM_Lock,
        ReadingRootDir,
        ReadingDetails,
        Complete,
        Failed
    };

    struct NodeScanState {
        uint8_t nodeId{0xFF};
        NodeState state{NodeState::Idle};
        FwSpeed currentSpeed{FwSpeed::S100};
        uint8_t retriesLeft{0};
        ConfigROM partialROM{};

        bool needsIRMCheck{false};
        bool irmCheckReadDone{false};
        bool irmCheckLockDone{false};
        bool irmIsBad{false};
        uint32_t irmBitBucket{0xFFFFFFFF};

        bool bibInProgress{false};
    };

    void AdvanceFSM();

    void OnBIBComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    void OnIRMReadComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    void OnIRMLockComplete(uint8_t nodeId, const ROMReader::ReadResult& result);

    void HandleIRMLockResult(NodeScanState& node, const ROMReader::ReadResult& result);

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

    void DiscoverDetails(uint8_t nodeId, uint32_t rootDirStart, std::vector<uint32_t> rootDirBE);
    void DiscoverVendorName(uint8_t nodeId, uint32_t rootDirStart, 
                            const std::vector<DirEntry>& rootEntries,
                            std::optional<DescriptorRef> vendorRef,
                            std::optional<DescriptorRef> modelRef,
                            std::vector<uint32_t> unitDirRelOffsets);
    void DiscoverModelName(uint8_t nodeId, uint32_t rootDirStart,
                           std::optional<DescriptorRef> modelRef,
                           std::vector<uint32_t> unitDirRelOffsets);
    void DiscoverUnitDirectories(uint8_t nodeId, uint32_t rootDirStart,
                                 std::vector<uint32_t> unitDirRelOffsets, size_t index);
    void FinalizeNodeDiscovery(uint8_t nodeId);

    void FetchTextDescriptor(uint8_t nodeId, uint32_t absOffset, uint8_t keyType,
                             std::function<void(std::string)> completion);
    void FetchTextLeaf(uint8_t nodeId, uint32_t absLeafOffset, std::function<void(std::string)> completion);
    void FetchDescriptorDirText(uint8_t nodeId, uint32_t absDirOffset, std::function<void(std::string)> completion);

    std::vector<DirEntry> ParseDirectory(const std::vector<uint32_t>& dirBE, uint32_t entryCap);
    std::optional<DescriptorRef> FindDescriptorRef(const std::vector<DirEntry>& entries, uint8_t ownerKeyId);

    void RetryWithFallback(NodeScanState& node);

    bool HasCapacity() const;

    void CheckAndNotifyCompletion();

    Async::IFireWireBus& bus_;
    SpeedPolicy& speedPolicy_;
    ROMScannerParams params_;
    std::unique_ptr<ROMReader> reader_;
    OSSharedPtr<IODispatchQueue> dispatchQueue_;

    Generation currentGen_{0};
    Driver::TopologySnapshot currentTopology_;
    std::vector<NodeScanState> nodeScans_;
    std::vector<ConfigROM> completedROMs_;
    uint16_t inflightCount_{0};

    ScanCompletionCallback onScanComplete_;
    bool completionNotified_{false};
    bool hadBusyNodes_{false};  // Set when any node returns ack_busy_X or BIB quadlet[0]=0

    Driver::TopologyManager* topologyManager_{nullptr};

    void ScheduleAdvanceFSM();
    void DispatchAsync(void (^work)());

    [[nodiscard]] static constexpr uint32_t RootDirStartQuadlet(const BusInfoBlock& bib) noexcept {
        return 1u + static_cast<uint32_t>(bib.busInfoLength);
    }

    [[nodiscard]] static constexpr uint32_t RootDirStartBytes(const BusInfoBlock& bib) noexcept {
        return RootDirStartQuadlet(bib) * 4u;
    }

    void EnsurePrefix(uint8_t nodeId, uint32_t requiredTotalQuadlets, std::function<void(bool)> completion);
};

} // namespace ASFW::Discovery
