//
// AVCUnit.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Unit - wraps Discovery::FWUnit with AV/C-specific functionality
// Owns FCPTransport, provides high-level command API, caches probe results
//

#pragma once

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#endif
#include <memory>
#include <atomic>
#include <span>
#include <vector>
#include "FCPTransport.hpp"
#include "Subunit.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"
#include "Core/AvcUnitModel.hpp"
#include "Core/IAvcUnit.hpp"

namespace ASFW::Protocols::AVC {

class AVCDiscovery;

enum class AVCDiscoveryStatus : uint8_t {
    Idle = 0, Running = 1, Completed = 2, Failed = 3, Skipped = 4
};

//==============================================================================
// Forward Declarations
//==============================================================================
class DescriptorAccessor;
namespace Graph { struct DeviceGraph; struct StreamGraph; }

//==============================================================================
// Unit Descriptor Information (Phase 5 Discovery)
//==============================================================================

/// Information extracted from Unit Identifier Descriptor
/// Ref: TA Document 2002013 Section 6.2.1
struct UnitDescriptorInfo {
    // Descriptor sizes from Unit Identifier
    uint8_t generationID{0};
    uint8_t sizeOfListID{0};
    uint8_t sizeOfObjectID{0};
    uint8_t sizeOfEntryPosition{0};

    // Root object lists
    uint16_t numberOfRootObjectLists{0};
    std::vector<uint64_t> rootListIDs;  // Variable-size IDs

    // Traversed root list contents (object IDs in each list)
    struct RootListContents {
        uint64_t listID;
        std::vector<uint64_t> objectIDs;
    };
    std::vector<RootListContents> rootListContents;

    // Support status
    bool descriptorMechanismSupported{false};
};

//==============================================================================
// AV/C Unit
//==============================================================================

class AVCUnit;

/// One unit isochronous plug formation, learned outside the music subunit (a
/// chip's format list). Directions are the host's: playback = unit ISO input.
struct UnitPlugFormation {
    uint32_t rateHz{0};
    uint32_t pcmChannels{0};
    uint32_t midiChannels{0};
};

// What discovery runs beyond the standard AV/C commands. The unit's owner sets
// it from the catalog plan; the unit itself never reads the catalog.
struct AVCUnitDiscoveryOptions {
    // The chip's read-only extension inventory (BridgeCo, Oxford). Runs after
    // the generic discovery and before the discovery status completes, at
    // attach and on every refresh; it must call `done` exactly once.
    std::function<void(AVCUnit& unit, std::function<void()> done)> extensionInventory;
    // Which stream-format opcode the chip is asked with (BridgeCo: 0x2F only).
    ASFW::AVC::IAvcUnit::StreamFormatOpcodePolicy streamFormatOpcode{
        ASFW::AVC::IAvcUnit::StreamFormatOpcodePolicy::kLearn};
};

class AVCUnit : public std::enable_shared_from_this<AVCUnit>,
                public ASFW::AVC::IAvcUnit {
public:
    using DiscoveryOptions = AVCUnitDiscoveryOptions;

    AVCUnit(std::shared_ptr<Discovery::FWDevice> device,
            std::shared_ptr<Discovery::FWUnit> unit,
            Discovery::DeviceRegistry& routeRegistry,
            Protocols::Ports::FireWireBusOps& busOps,
            Protocols::Ports::FireWireBusInfo& busInfo,
            Scheduling::ITimerScheduler& timerScheduler,
            DiscoveryOptions options = {});

    ~AVCUnit() override;

    AVCUnit(const AVCUnit&) = delete;
    AVCUnit& operator=(const AVCUnit&) = delete;

    // --- IAvcUnit implementation ---
    void Submit(const ASFW::AVC::CommandFrame& frame,
                FW::Generation generation,
                ResponseCallback completion) override;

    [[nodiscard]] FW::NodeId NodeId() const noexcept override;
    [[nodiscard]] FW::Generation CurrentGeneration() const noexcept override;
    [[nodiscard]] uint64_t Guid() const noexcept override;

    void Initialize(std::function<void(bool success)> completion);

    void ReScan(std::function<void(bool success)> completion);
    [[nodiscard]] AVCDiscoveryStatus GetDiscoveryStatus() const noexcept {
        return discoveryStatus_.load(std::memory_order_acquire);
    }
    void MarkRescanSkipped() noexcept {
        if (rescanInProgress_.load(std::memory_order_acquire)) return;
        auto status = discoveryStatus_.load(std::memory_order_acquire);
        while (status != AVCDiscoveryStatus::Running &&
               !discoveryStatus_.compare_exchange_weak(status, AVCDiscoveryStatus::Skipped,
                                                       std::memory_order_acq_rel)) {}
    }
    [[nodiscard]] bool TryBeginRescan() noexcept {
        bool expected = false;
        if (!rescanInProgress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        discoveryStatus_.store(AVCDiscoveryStatus::Running, std::memory_order_release);
        return true;
    }
    /// End a rescan begun with TryBeginRescan() that a family bring-up ran
    /// instead of Initialize (BeBoB plug probes).
    void FinishExternalRescan(bool success) noexcept {
        discoveryStatus_.store(success ? AVCDiscoveryStatus::Completed : AVCDiscoveryStatus::Failed,
                               std::memory_order_release);
        rescanInProgress_.store(false, std::memory_order_release);
    }
    /// Start a new exchange log for this unit (manual refresh).
    void BeginExchangeSession() { if (fcpTransport_) fcpTransport_->BeginExchangeSession(); }
    /// Every FCP exchange with this unit since the session started.
    [[nodiscard]] FcpExchangeLog CopyExchangeLog() const {
        return fcpTransport_ ? fcpTransport_->CopyExchangeLog() : FcpExchangeLog{};
    }
    void ProbeUnitInfo(std::function<void(bool)> completion);


    void GetPlugInfo(std::function<void(AVCResult, const ASFW::AVC::Cmd::UnitPlugCounts&)> completion);

    const ASFW::AVC::Cmd::UnitPlugCounts& GetCachedPlugCounts() const { return model_.unitPlugs; }
    const ASFW::AVC::UnitModel& GetModel() const noexcept { return model_; }
    std::shared_ptr<const Graph::DeviceGraph> GetDiscoveredGraph() const noexcept { return discoveredGraph_; }
    /// Size the graph's selected streams from the unit's live plug formations
    /// at its current rate. A device whose music subunit rejects the
    /// current-format query (Phase 88) still lists them, and they win over any
    /// size the music subunit gave; a stream with no matching formation keeps
    /// what it had.
    void CompleteGraphFromUnitPlugFormations(std::span<const UnitPlugFormation> playback,
                                             std::span<const UnitPlugFormation> capture,
                                             uint32_t currentRateHz);
    ASFW::AVC::UnitModel& GetModel() noexcept { return model_; }

    const std::vector<std::shared_ptr<Subunit>>& GetSubunits() const { return subunits_; }

    const UnitDescriptorInfo& GetDescriptorInfo() const { return descriptorInfo_; }

    std::shared_ptr<Discovery::FWUnit> GetFWUnit() const { return unit_.lock(); }

    std::shared_ptr<Discovery::FWDevice> GetDevice() const { return device_.lock(); }

    FCPTransport& GetFCPTransport() { return *fcpTransport_; }
    const FCPTransport& GetFCPTransport() const { return *fcpTransport_; }
    std::shared_ptr<FCPTransport> GetFCPTransportShared() const { return fcpTransport_; }

    void OnBusReset(uint32_t newGeneration);
    void OnRouteRevalidated();

    /// Stop the unit's FCP transport before the async bus is torn down.
    void Shutdown();

    bool IsInitialized() const { return initialized_; }

    uint64_t GetGUID() const;

    uint32_t GetSpecID() const;

private:
    friend class AVCDiscovery;
    void InitializeAlreadyBegun(std::function<void(bool success)> completion);
    void ReScanAlreadyBegun(std::function<void(bool success)> completion);

    void ProbeDescriptorMechanism(std::function<void(bool)> completion);

    bool ParseUnitIdentifier(const std::vector<uint8_t>& data);

    void TraverseRootLists(size_t listIndex, std::function<void(bool)> completion);

    void ReadRootObjectList(uint64_t listID,
                           std::function<void(bool success, std::vector<uint64_t> objectIDs)> completion);

    void ProbeSubunits(std::function<void(bool)> completion);

    void ProbePlugs(std::function<void(bool)> completion);

    void PopulateKnownSubunitPlugCounts();

    void ResolveDiscoveredGraph(std::function<void(bool)> completion);
    void ResolveUnitStreamGraph(std::function<void(bool)> completion);
    /// Size a selected stream from a format found outside the music subunit,
    /// keeping its descriptor names and validating its slot map at that width.
    [[nodiscard]] bool CompleteStream(Graph::StreamGraph& stream, uint32_t pcmChannels,
                                      uint32_t midiChannels, uint32_t rateHz,
                                      std::vector<uint32_t> rates) const;

    void ProbeSignalFormat(std::function<void(bool)> completion);

    void StoreSubunitInfo(const ASFW::AVC::Cmd::SubunitInfo& info);

    void ParseSubunitCapabilities(size_t index, std::function<void(bool)> completion);

    std::weak_ptr<Discovery::FWDevice> device_;
    std::weak_ptr<Discovery::FWUnit> unit_;
    Discovery::DeviceRegistry& routeRegistry_;

    Protocols::Ports::FireWireBusOps& busOps_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    Scheduling::ITimerScheduler& timerScheduler_;
    const DiscoveryOptions options_;

    std::shared_ptr<FCPTransport> fcpTransport_;

    std::shared_ptr<DescriptorAccessor> descriptorAccessor_;

    std::vector<std::shared_ptr<Subunit>> subunits_;
    ASFW::AVC::UnitModel model_{};
    std::shared_ptr<const Graph::DeviceGraph> discoveredGraph_{};
    UnitDescriptorInfo descriptorInfo_;

    bool initialized_{false};
    std::atomic<AVCDiscoveryStatus> discoveryStatus_{AVCDiscoveryStatus::Idle};
    std::atomic<bool> rescanInProgress_{false};
};

} // namespace ASFW::Protocols::AVC
